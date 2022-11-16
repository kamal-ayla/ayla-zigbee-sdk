from . import ExporterModel
from . import EUSART_model
from . import RuntimeModel


class EUSART(ExporterModel.Module):
    def __init__(self, name=None):
        if not name:
            name = self.__class__.__name__
        super(EUSART, self).__init__(name, visible=True, core=True)
        self.model = EUSART_model

    def set_runtime_hooks(self):
        pass
