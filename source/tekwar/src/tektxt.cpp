#include "build.h"
#include "baselayer.h"
#include "cache1d.h"
#include "mmulti.h"

#include "tekwar.h"

#if 0

/***************************************************************************
 *   TEKTXT.C  - text based & MiSCcellaneous routines for Tekwar game      *
 *   Uses BIOS modes for compatibility and scrolling.                      *
 *   Includes CD code.                                                     *
 *                                                     12/14/94 Les Bird   *
 ***************************************************************************/

#define   CDRELEASE
#define   COPYRIGHTSTRING     "INTRACORP1995TW"

#endif

char      bypasscdcheck=0;

void
crash(const char *s,...)
{
     va_list argptr;

     // print the message first: if any shutdown step dies on partially
     // initialized state, the actual error must not be lost
     va_start(argptr,s);
     vprintf(s,argptr);
     va_end(argptr);
     printf("\n");
     fflush(stdout);

     musicoff();
     sendlogoff();
     uninitmultiplayers();
     uninitsb();
     cduninit();
     //uninittimer();
     uninitinput();
     engineUnInit();
     uninitgroupfile();
     teksavesetup();

     exit(0);
}
