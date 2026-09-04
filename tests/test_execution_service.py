"""Transport, admission, expiry and drain contracts without model dependencies."""

import time
from threading import Thread
from urllib.error import HTTPError
from urllib.request import Request, urlopen

import numpy as np
import pytest

from flashrt_nexus.execution import ExecutionHTTPServer, ExecutionService, ServiceError
from flashrt_nexus.remote import RemoteExecutionError, RemoteProvider
from serve.transports import wire


@pytest.fixture
def service():
    instance = ExecutionService("worker_fixture:build", {}, lease_timeout_s=1)
    try:
        yield instance
    finally:
        instance.close()


def wait_reset(service):
    deadline = time.monotonic() + 5
    while service.health()["resetting"]:
        assert time.monotonic() < deadline
        time.sleep(0.01)


def wait_result(service, body):
    deadline = time.monotonic() + 5
    while True:
        result = service.result(body)
        if result["state"] != "pending":
            return result
        assert time.monotonic() < deadline
        time.sleep(0.01)


def test_admission_retained_results_reset_and_expiry(service):
    lease = service.acquire()
    body = {**lease, "request": "first", "inputs": {"value": 3, "delay": 0.1}}
    service.submit(body)
    with pytest.raises(ServiceError, match="busy"):
        service.acquire()
    with pytest.raises(ServiceError, match="busy"):
        service.submit(body)
    expected = wait_result(service, body)
    np.testing.assert_array_equal(expected["output"], 3)
    np.testing.assert_array_equal(service.result(body)["output"], 3)
    service.acknowledge(body)
    with pytest.raises(ServiceError, match="stale_request"):
        service.result(body)
    service.submit({**body, "request": "old", "inputs": {"delay": 0.1, "value": 99}})
    service.reset(lease)
    wait_reset(service)
    with pytest.raises(ServiceError, match="stale_request"):
        service.result({**lease, "request": "old"})
    service.submit({**body, "request": "new", "inputs": {}})
    np.testing.assert_array_equal(wait_result(service, {**lease, "request": "new"})["output"], 1)
    service._seen -= 2
    service.maintain()
    with pytest.raises(ServiceError, match="stale_session"):
        service.result({**lease, "request": "new"})
    wait_reset(service)
    assert service.acquire()["lease"] != lease["lease"]


def test_provider_failure_and_epoch_mismatch(service):
    lease = service.acquire()
    body = {**lease, "request": "failure", "inputs": {"fail": True}}
    service.submit(body)
    result = wait_result(service, body)
    assert result["state"] == "error"
    assert result["provider_error_type"] == "ValueError"
    with pytest.raises(ServiceError, match="stale_session"):
        service.result({**body, "epoch": "old-service"})


@pytest.fixture
def endpoint(service):
    server = ExecutionHTTPServer(("127.0.0.1", 0), service, token="test-token")
    thread = Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01})
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_port}"
    finally:
        server.shutdown()
        thread.join()
        server.server_close()


def test_http_roundtrip_auth_reset_and_deadline(endpoint, monkeypatch):
    with pytest.raises(HTTPError) as denied:
        urlopen(endpoint + "/healthz", timeout=1)
    assert denied.value.code == 401
    monkeypatch.setenv("NEXUS_TEST_TOKEN", "test-token")
    client = RemoteProvider({"url": endpoint, "token_env": "NEXUS_TEST_TOKEN",
                             "execution_timeout_s": 0.1})
    try:
        assert client.describe()["action_shape"] == [3, 2]
        np.testing.assert_array_equal(client.execute({"value": 4}), 4)
        with pytest.raises(TimeoutError, match="deadline"):
            client.execute({"value": 99, "delay": 0.3})
        client.execution_timeout = 5
        client.reset()
        np.testing.assert_array_equal(client.execute({}), 1)
        with pytest.raises(RemoteExecutionError, match="ValueError"):
            client.execute({"fail": True})
    finally:
        client.close()


def test_http_malformed_body_and_public_binding_guard(endpoint, service):
    request = Request(endpoint + "/v1/execution/acquire", data=b"[]",
                      headers={"Authorization": "Bearer test-token"})
    with pytest.raises(HTTPError) as invalid:
        urlopen(request, timeout=1)
    assert invalid.value.code == 400
    with pytest.raises(ValueError, match="API token"):
        ExecutionHTTPServer(("0.0.0.0", 0), service)


def test_wire_arrays_and_malformed_shapes():
    arrays = [np.arange(12, dtype=np.uint8).reshape(2, 2, 3),
              np.array([0.25, 0.5], dtype=np.float32)]
    result = wire.loads(wire.dumps({"images": arrays, "prompt": "pick"}))
    for before, after in zip(arrays, result["images"]):
        np.testing.assert_array_equal(before, after)
        assert before.dtype == after.dtype
    with pytest.raises(ValueError, match="dtype"):
        wire.dumps(np.array([object()], dtype=object))
    with pytest.raises(ValueError, match="byte count"):
        wire.loads(b'{"__nexus_array__":"", "dtype":"<f4", "shape":[2]}')
    with pytest.raises(ValueError, match="non-finite"):
        wire.loads(b'{"value":NaN}')


def test_lost_submit_response_requires_explicit_reset(endpoint, monkeypatch):
    monkeypatch.setenv("NEXUS_TEST_TOKEN", "test-token")
    client = RemoteProvider({"url": endpoint, "token_env": "NEXUS_TEST_TOKEN"})
    call = client._call
    def lost_reply(method, body):
        result = call(method, body)
        if method == "submit":
            raise RemoteExecutionError("injected lost response")
        return result
    try:
        client._call = lost_reply
        with pytest.raises(RemoteExecutionError, match="lost response"):
            client.execute({"value": 99, "delay": 0.1})
        client._call = call
        with pytest.raises(RemoteExecutionError, match="409"):
            client.execute({"value": 2})
        client.reset()
        np.testing.assert_array_equal(client.execute({}), 1)
    finally:
        client.close()


def test_failed_reset_never_admits_new_work():
    service = ExecutionService("worker_fixture:build", {"fail_reset": True})
    try:
        lease = service.acquire()
        service.release(lease)
        wait_reset(service)
        assert not service.health()["ready"]
        with pytest.raises(ServiceError, match="unavailable"):
            service.acquire()
    finally:
        service.close()
