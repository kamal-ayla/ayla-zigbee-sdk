// This callback file is created for your convenience. You may add application
// code to this file. If you regenerate this file over a previous version, the
// previous version will be overwritten and any code you have added will be
// lost.

#include "app/framework/include/af.h"
#include "app/framework/util/util.h"    // emberAfGetPageFrom8bitEncodedChanPg()



/** 
 * Custom CLI.  This command tree is executed by typing "custom <command>"
 * See app/util/serial/command-interpreter2.h for more detail on writing commands.
**/
/*  Example sub-menu */
//  extern void doSomethingFunction(void);
//  static EmberCommandEntry customSubMenu[] = {
//    emberCommandEntryAction("do-something", doSomethingFunction, "", "Do something description"),
//    emberCommandEntryTerminator()
//  };

//  extern void actionFunction(void);
EmberCommandEntry emberAfCustomCommands[] = {
  /* Sample Custom CLI commands */
  // emberCommandEntrySubMenu("sub-menu", customSubMenu, "Sub menu of custom commands"),
  // emberCommandEntryAction("action", actionFunction, "", "Description of action command."),
  emberCommandEntryTerminator()
};


