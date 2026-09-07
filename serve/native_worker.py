"""Expose the existing model-runtime session to an execution worker."""

from .embedded import EmbeddedSession


class NativeWorkerProvider:
    def __init__(self, config):
        self.config = config
        self.session = EmbeddedSession.open(config)

    def describe(self):
        return {"provider": "model_runtime",
                "action_shape": self.session.state()["action_shape"]}

    def execute(self, inputs):
        return self.session.act(**inputs).actions

    def reset(self):
        # A new deployment is the generic reset boundary; no capsule or
        # producer-specific state capability is invented for opaque runtimes.
        self.session.close()
        self.session = None
        self.session = EmbeddedSession.open(self.config)

    def close(self):
        if self.session is not None:
            self.session.close()


def build(config):
    return NativeWorkerProvider(config)
