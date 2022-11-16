from . import ExporterModel
from . import RSSI_model
from . import RuntimeModel


class RSSI(ExporterModel.Module):
    def __init__(self, name=None):
        if not name:
            name = self.__class__.__name__
        super(RSSI, self).__init__(name, visible=True)
        self.model = RSSI_model