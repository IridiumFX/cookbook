/*
 * alforno.h — cookbook's handler-facing alforno API.
 *
 * Redirects to the alforno_compat shim, which implements the sibling-project
 * alf_* / AlfContext / ALF_* API on top of apennines `t4/pasta/alforno`.
 * Cookbook code compiles unchanged; the alforno_compat.c wrappers translate
 * each call to the apennines native alforno_ctx / alforno_op shape.
 *
 * History: in rc4 the alforno submodule was replaced by apennines' native
 * implementation, with alforno_compat as the thin adapter.
 */
#ifndef ALFORNO_H
#define ALFORNO_H

#include "alforno_compat.h"

#endif /* ALFORNO_H */
