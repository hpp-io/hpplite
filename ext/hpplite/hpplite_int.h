/*
** HPPLite - internal interface between the extension's source files.
**
** hpplite.h is what applications and node.c see; this header is what the
** files of the capture engine share with each other: the context (hpplite.h),
** and the functions one file defines for another. Everything declared here
** is internal: the names may change with the files.
*/
#ifndef HPPLITE_INT_H
#define HPPLITE_INT_H

#include "hpplite.h"
#include "sha256.h"
#include "capture_rows.h"
#include "clock.h"
#include "batch.h"

#endif /* HPPLITE_INT_H */
