from .upgradeUtility import *

description="BSP_COEX_DP_RACLNAEN_INV_CHANNEL is being deprecated and so removed from hwconf files."
version="3.1.1"
priority=1
def upgradeCallback(xmlDevice, verbose=False):
	verbose = True
	if verbose:
		print ("%s upgradeCallback" % __name__)
	newXmlDevice = removePropertyLine(xmlDevice, "COEX.BSP_COEX_DP_RACLNAEN_INV_CHANNEL.ENUM", verbose)
	return newXmlDevice