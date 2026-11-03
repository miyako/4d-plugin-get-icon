#include "example-4dplugin.h"

void PluginMain(PA_long32 selector, PA_PluginParameters params) {
    
    switch(selector)
    {
        case 1 :
            example_greeting(params);
            break;

    }
}

static void example_greeting(PA_PluginParameters params) {

    // get arguments
    PA_Unistring *name = PA_GetStringParameter(params, 1);
    PA_long32 greetingType = PA_GetLongParameter(params, 2);

    // determine greeting type from system time if default (0)
    if (greetingType == 0) {
        time_t now = time(NULL);
        struct tm local;
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        local = *localtime(&now);
#endif
        int hour = local.tm_hour;
        if (hour >= 3 && hour < 12) {
            greetingType = 1; // morning
        } else if (hour >= 12 && hour < 18) {
            greetingType = 2; // afternoon
        } else {
            greetingType = 3; // evening
        }
    }

    // select greeting prefix (ASCII, safe to widen to UTF-16)
    const char *prefix;
    switch (greetingType) {
        case 1:  prefix = "Good morning "; break;
        case 2:  prefix = "Good afternoon "; break;
        case 3:  prefix = "Good evening "; break;
        default: prefix = "Good morning "; break;
    }

    // get name data
    PA_Unichar *nameChars = PA_GetUnistring(name);
    PA_long32 nameLen = PA_GetUnistringLength(name);
    PA_long32 prefixLen = (PA_long32)strlen(prefix);

    // build result buffer (prefix + name + null terminator)
    PA_long32 resultLen = prefixLen + nameLen;
    PA_Unichar result[1024];

    // widen ASCII prefix to UTF-16
    PA_long32 i;
    for (i = 0; i < prefixLen && i < 1023; i++) {
        result[i] = (PA_Unichar)prefix[i];
    }

    // append name
    PA_long32 j;
    for (j = 0; j < nameLen && (i + j) < 1023; j++) {
        result[i + j] = nameChars[j];
    }
    result[i + j] = 0;
    
    PA_ReturnString(params, result);
}
