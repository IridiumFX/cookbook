#ifndef COOKBOOK_H
#define COOKBOOK_H

#ifdef COOKBOOK_STATIC
  #define COOKBOOK_API
#elif defined(_WIN32)
  #ifdef BUILDING_COOKBOOK
    #define COOKBOOK_API __declspec(dllexport)
  #else
    #define COOKBOOK_API __declspec(dllimport)
  #endif
#elif defined(__GNUC__) || defined(__clang__)
  #define COOKBOOK_API __attribute__((visibility("default")))
#else
  #define COOKBOOK_API
#endif

COOKBOOK_API int cookbook_version_major(void);
COOKBOOK_API int cookbook_version_minor(void);
COOKBOOK_API int cookbook_version_patch(void);

#endif /* COOKBOOK_H */
