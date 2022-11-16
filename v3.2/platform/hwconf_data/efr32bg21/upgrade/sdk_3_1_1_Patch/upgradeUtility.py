# SDK 3.1.1 Patch upgrade utilities

# Remove property with propertyId
# Note this function returns an xmldevice which should be passed back up to the
# upgradeDispatch level. Example:
#   newXmlDevice = removePropertyLine(xmlDevice, "CMU.HAL_CLK_HFCLK_SOURCE.ENUM", verbose)
#   return newXmlDevice
def removePropertyLine(xmldevice, propertyId, verbose=False):
  xmlmode = xmldevice.getModes()[0]
  properties = xmlmode.getProperty()
  removeList = []
  for i in range(0,len(properties)):
    p = properties[i]
    if propertyId == p.getPropertyId():
      if verbose:
        print ("Removing %s" % (propertyId))
      removeList.append(i)
  # reverse sort indicies in removeList so they can be removed from last to first
  for index in sorted(removeList, reverse=True):
    xmlmode.removeProperty(index)
  return xmldevice
