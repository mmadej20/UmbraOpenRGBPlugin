/*---------------------------------------------------------*\
| UmbraPlugin.h                                             |
|                                                           |
|   OpenRGB Plugin API v4 entry point for the AsiaHorse     |
|   UMBRA ARGB Hub plugin. Detects UMBRA hubs over USB      |
|   HID and registers them as standard RGBControllers.      |
|                                                           |
|   This file is part of the UmbraOpenRGBPlugin project.    |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <QImage>
#include <QMenu>
#include <QObject>
#include <QWidget>

#include <mutex>
#include <string>
#include <vector>

#include "OpenRGBPluginInterface.h"
#include "UmbraController.h"
#include "UmbraWidget.h"

class RGBController_Umbra;

#ifndef UMBRA_PLUGIN_VERSION
#define UMBRA_PLUGIN_VERSION "1.0.0"
#endif

class UmbraPlugin : public QObject, public OpenRGBPluginInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID OpenRGBPluginInterface_IID)
    Q_INTERFACES(OpenRGBPluginInterface)

public:
    UmbraPlugin();
    virtual ~UmbraPlugin();

    /*-----------------------------------------------------*\
    | OpenRGBPluginInterface                                |
    \*-----------------------------------------------------*/
    OpenRGBPluginInfo GetPluginInfo() override;
    unsigned int GetPluginAPIVersion() override;

    void Load(ResourceManagerInterface* resource_manager_ptr) override;
    QWidget* GetWidget() override;
    QMenu* GetTrayMenu() override;
    void Unload() override;

private slots:
    void Rescan();

private:
    /*-----------------------------------------------------*\
    | How many consecutive enumerations may miss a hub      |
    | before its entry is dropped (transient-scan guard)    |
    \*-----------------------------------------------------*/
    static const unsigned int MISSED_SCANS_BEFORE_DROP = 2;

    /*-----------------------------------------------------*\
    | One hub: HID transport plus (optionally) the          |
    | registered OpenRGB controller exposing it             |
    \*-----------------------------------------------------*/
    struct HubEntry
    {
        UmbraController*        transport;
        RGBController_Umbra*    controller;     /* null until exposed in OpenRGB */

        /*-----------------------------------------------------*\
        | Consecutive enumerations that did NOT list this      |
        | hub's path. A single miss can be transient (device   |
        | busy re-enumerating during the scan); the entry is   |
        | only dropped after several misses in a row           |
        \*-----------------------------------------------------*/
        unsigned int            missed_scans;
    };

    void DetectControllers();
    void RemoveControllers();

    void UpdateWidgetStatus();

    static void DetectionEndCallback(void* this_ptr);

    ResourceManagerInterface*   rm_;

    UmbraWidget*                widget_;

    std::vector<HubEntry>       hubs_;
    std::mutex                  mutex_;
};
