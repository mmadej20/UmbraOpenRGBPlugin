/*---------------------------------------------------------*\
| plugin_load_test.cpp                                      |
|                                                           |
|   Loads the built plugin DLL through QPluginLoader and    |
|   validates the metadata + API boundary crossing.         |
|                                                           |
|   A successful compile/link does NOT prove the DLL can be |
|   loaded: with statically embedded libstdc++/libgcc/      |
|   winpthread this is exactly where ABI problems would     |
|   surface first (missing imports, mismatched vtables,     |
|   qobject_cast failures).                                 |
|                                                           |
|   This test intentionally never touches HID hardware.     |
|                                                           |
|   This file is part of the UmbraOpenRGBPlugin project.    |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "OpenRGBPluginInterface.h"

#include <QCoreApplication>
#include <QPluginLoader>
#include <QString>

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if(!(cond))                                                         \
        {                                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            g_failures++;                                                   \
        }                                                                   \
    } while(0)

int main(int argc, char** argv)
{
    /*-----------------------------------------------------*\
    | QCoreApplication is enough for QPluginLoader; no      |
    | widget is ever instantiated here                      |
    \*-----------------------------------------------------*/
    QCoreApplication app(argc, argv);

    QString dll_path;

    if(argc > 1)
    {
        dll_path = QString::fromLocal8Bit(argv[1]);
    }
    else
    {
        dll_path = QCoreApplication::applicationDirPath()
                 + "/UmbraOpenRGBPlugin.dll";
    }

    printf("Loading '%s'...\n", dll_path.toLocal8Bit().constData());

    QPluginLoader loader(dll_path);

    /*-----------------------------------------------------*\
    | Step 1: the DLL must load and expose its QObject      |
    \*-----------------------------------------------------*/
    QObject* object = loader.instance();

    if(object == nullptr)
    {
        printf("FAIL loader.errorString(): %s\n",
               loader.errorString().toLocal8Bit().constData());
        g_failures++;
    }

    CHECK(object != nullptr);

    if(object == nullptr)
    {
        return (g_failures == 0) ? 0 : 1;
    }

    /*-----------------------------------------------------*\
    | Step 2: Qt metadata cast across the DLL boundary      |
    \*-----------------------------------------------------*/
    OpenRGBPluginInterface* plugin =
        qobject_cast<OpenRGBPluginInterface*>(object);

    CHECK(plugin != nullptr);

    if(plugin == nullptr)
    {
        return (g_failures == 0) ? 0 : 1;
    }

    /*-----------------------------------------------------*\
    | Step 3: API version and metadata content              |
    \*-----------------------------------------------------*/
    CHECK(plugin->GetPluginAPIVersion() == OPENRGB_PLUGIN_API_VERSION);
    CHECK(OPENRGB_PLUGIN_API_VERSION == 4);

    OpenRGBPluginInfo info = plugin->GetPluginInfo();

    CHECK(info.Name == "AsiaHorse UMBRA");
    CHECK(!info.Description.empty());
    CHECK(info.Version == UMBRA_PLUGIN_VERSION_EXPECTED);
    CHECK(info.Location == OPENRGB_PLUGIN_LOCATION_DEVICES);
    CHECK(info.Label == "UMBRA");

    /*-----------------------------------------------------*\
    | Step 4: tray menu may legitimately be null            |
    \*-----------------------------------------------------*/
    CHECK(plugin->GetTrayMenu() == nullptr);

    /*-----------------------------------------------------*\
    | Step 5: Load(nullptr) / Unload() smoke test.          |
    | The plugin guards against a null ResourceManager, so  |
    | this exercises the lifecycle paths without hardware   |
    \*-----------------------------------------------------*/
    plugin->Load(nullptr);
    plugin->Unload();

    /*-----------------------------------------------------*\
    | Step 6: clean unload of the library itself            |
    \*-----------------------------------------------------*/
    CHECK(loader.unload());

    if(g_failures == 0)
    {
        printf("PLUGIN LOAD TEST PASSED (%s)\n",
               info.Version.c_str());
        return 0;
    }

    printf("%d FAILURE(S)\n", g_failures);

    return 1;
}
