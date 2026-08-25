/*---------------------------------------------------------*\
| RGBController_Umbra.h                                     |
|                                                           |
|   OpenRGB device abstraction for the AsiaHorse UMBRA      |
|   ARGB Hub.  Each populated physical port becomes an      |
|   independent linear zone driven in Direct mode.          |
|                                                           |
|   This file is part of the UmbraOpenRGBPlugin project.    |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include "RGBController.h"
#include "UmbraController.h"

class RGBController_Umbra : public RGBController
{
public:
    /*-----------------------------------------------------*\
    | The transport is owned by the plugin, not by this     |
    | controller. The plugin guarantees that it deletes     |
    | the transport only after this controller has been     |
    | destroyed (which joins the update thread).            |
    \*-----------------------------------------------------*/
    RGBController_Umbra(UmbraController* controller);
    virtual ~RGBController_Umbra();

    void SetupZones() override;
    void ResizeZone(int zone, int new_size) override;

    void DeviceUpdateLEDs() override;
    void UpdateZoneLEDs(int zone) override;
    void UpdateSingleLED(int led) override;

    void DeviceUpdateMode() override;
    void DeviceSaveMode() override;

private:
    /*-----------------------------------------------------*\
    | Packs OpenRGB's flat color array into protocol order  |
    | and pushes a full frame to the hub                    |
    \*-----------------------------------------------------*/
    bool SendFullFrame();

    UmbraController*        umbra_;
};
