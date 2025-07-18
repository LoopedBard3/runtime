// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#ifndef _JITCONFIG_H_
#define _JITCONFIG_H_

#include "switches.h"

struct CORINFO_SIG_INFO;
typedef struct CORINFO_CLASS_STRUCT_*  CORINFO_CLASS_HANDLE;
typedef struct CORINFO_METHOD_STRUCT_* CORINFO_METHOD_HANDLE;
class ICorJitHost;

class JitConfigValues
{
public:
    class MethodSet
    {
    private:
        struct MethodName
        {
            MethodName* m_next;
            const char* m_patternStart;
            const char* m_patternEnd;
            bool        m_containsAssemblyName;
            bool        m_containsClassName;
            bool        m_classNameContainsInstantiation;
            bool        m_methodNameContainsInstantiation;
            bool        m_containsSignature;
        };

        const char* m_listFromConfig;
        MethodName* m_names;

        MethodSet(const MethodSet& other)            = delete;
        MethodSet& operator=(const MethodSet& other) = delete;

    public:
        MethodSet()
        {
        }

        const char* list() const
        {
            return m_listFromConfig;
        }

        void initialize(const char* listFromConfig, ICorJitHost* host);
        void destroy(ICorJitHost* host);

        inline bool isEmpty() const
        {
            return m_names == nullptr;
        }
        bool contains(CORINFO_METHOD_HANDLE methodHnd, CORINFO_CLASS_HANDLE classHnd, CORINFO_SIG_INFO* sigInfo) const;
    };

private:

#define RELEASE_CONFIG_INTEGER(name, key, defaultValue) int m_##name;
#define RELEASE_CONFIG_STRING(name, key)                const char* m_##name;
#define RELEASE_CONFIG_METHODSET(name, key)             MethodSet m_##name;

#include "jitconfigvalues.h"

public:
#define RELEASE_CONFIG_INTEGER(name, key, defaultValue)                                                                \
    inline int name() const                                                                                            \
    {                                                                                                                  \
        return m_##name;                                                                                               \
    }
#define RELEASE_CONFIG_STRING(name, key)                                                                               \
    inline const char* name() const                                                                                    \
    {                                                                                                                  \
        return m_##name;                                                                                               \
    }
#define RELEASE_CONFIG_METHODSET(name, key)                                                                            \
    inline const MethodSet& name() const                                                                               \
    {                                                                                                                  \
        return m_##name;                                                                                               \
    }

#include "jitconfigvalues.h"

private:
    bool m_isInitialized;

    JitConfigValues(const JitConfigValues& other)            = delete;
    JitConfigValues& operator=(const JitConfigValues& other) = delete;

public:
    JitConfigValues()
    {
    }

    inline bool isInitialized() const
    {
        return m_isInitialized != 0;
    }
    void initialize(ICorJitHost* host);
    void destroy(ICorJitHost* host);
};

extern JitConfigValues JitConfig;

#endif
