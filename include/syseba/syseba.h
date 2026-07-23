#ifndef SYSEBA_PUBLIC_H
#define SYSEBA_PUBLIC_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SYSEBA_VERSION
#define SYSEBA_VERSION "2.0.0"
#endif

#define SYSEBA_APP_NAME "SySeBa"
#define SYSEBA_APP_TITLE "SySeBa - The Syncro Service Backup"

const char *syseba_version(void);

#ifdef __cplusplus
}
#endif

#endif
