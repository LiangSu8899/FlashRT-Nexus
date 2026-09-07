"""Native host drain ordering without a device or model dependency."""

from types import SimpleNamespace
from threading import Lock
from unittest.mock import Mock

import pytest

from flashrt_nexus import ActionChunkSession


def controller():
    value = ActionChunkSession.__new__(ActionChunkSession)
    value.nx = Mock()
    value.nx.cap_model_n_stages.return_value = 2
    value.nx.cap_model_stage_executor_kind.return_value = 0
    value.nx.nexus_stage_dag_sync.return_value = 0
    value.session = SimpleNamespace(model=1, lock=Lock())
    value._dag = 1
    value._mode = 2
    value._closed = False
    return value


def test_reset_and_close_drain_before_releasing():
    value = controller()
    value.reset()
    names = [c[0] for c in value.nx.mock_calls]
    assert names[-1] == 'nexus_action_chunk_reset'
    assert names.count('nexus_stage_dag_sync') == 2
    value.nx.reset_mock()
    value.close()
    names = [c[0] for c in value.nx.mock_calls]
    assert names[-2:] == ['nexus_action_chunk_destroy', 'nexus_stage_dag_destroy']
    assert names.count('nexus_stage_dag_sync') == 2
    value.close()


def test_failed_drain_preserves_owners():
    value = controller()
    value.nx.nexus_stage_dag_sync.return_value = -1
    with pytest.raises(RuntimeError, match='drain'):
        value.reset()
    value.nx.nexus_action_chunk_reset.assert_not_called()
    with pytest.raises(RuntimeError, match='drain'):
        value.close()
    value.nx.nexus_action_chunk_destroy.assert_not_called()
    assert not value._closed
