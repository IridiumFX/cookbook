#ifndef APENNINES_EXPORT_H
#define APENNINES_EXPORT_H

#ifdef APENNINES_STATIC
    #define APENNINES_API
#elif defined(APENNINES_BUILDING)
    #ifdef _WIN32
        #define APENNINES_API __declspec(dllexport)
    #else
        #define APENNINES_API __attribute__((visibility("default")))
    #endif
#else
    #ifdef _WIN32
        #define APENNINES_API __declspec(dllimport)
    #else
        #define APENNINES_API
    #endif
#endif

#endif /* APENNINES_EXPORT_H */
