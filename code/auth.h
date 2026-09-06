#ifndef AUTH_H
#define AUTH_H

#include "globals.h"

void authBegin();
bool authIsAuthenticated();
bool authRequire();
bool authRequireCsrf();
void authInvalidateAll();
void handleApiSession();
void handleApiLogin();
void handleApiLogout();

#endif
