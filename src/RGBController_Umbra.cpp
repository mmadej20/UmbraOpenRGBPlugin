/*---------------------------------------------------------*\
| RGBController_Umbra.cpp                                   |
|                                                           |
|   OpenRGB device abstraction for the AsiaHorse UMBRA      |
|   ARGB Hub.                                               |
|                                                           |
|   This file is part of the UmbraOpenRGBPlugin project.    |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "RGBController_Umbra.h"

#include <cstdio>

RGBController_Umbra::RGBController_Umbra(UmbraController* controller)
    : umbra_(controller)
{
    name        = "AsiaHorse UMBRA";
    vendor      = "AsiaHorse";
    description = "AsiaHorse UMBRA ARGB Hub (10x ARGB)";
    version     = "1.0";
    serial      = umbra_->GetSerial();
    location    = umbra_->GetLocation();

    type = DEVICE_TYPE_LEDSTRIP;

    /*-----------------------------------------------------*\
    | Direct mode only - the hub has no onboard effect      |
    | engine exposed by the known protocol                  |
    \*-----------------------------------------------------*/
    mode direct;
    direct.name       = "Direct";
    direct.value      = 0x0000;
    direct.flags      = MODE_FLAG_HAS_PER_LED_COLOR;
    direct.color_mode = MODE_COLORS_PER_LED;
    modes.push_back(direct);

    active_mode = 0;

    SetupZones();
}

RGBController_Umbra::~RGBController_Umbra()
{
    /*-----------------------------------------------------*\
    | Detach from hardware before the base class joins the  |
    | update thread. The transport itself is owned by the   |
    | plugin and is deleted later, so in-flight calls that  |
    | are already past IsConnected() remain memory-safe:    |
    | they block on the transport's IO mutex until this     |
    | disconnect completes and then see a closed device.    |
    \*-----------------------------------------------------*/
    if(umbra_ != nullptr)
    {
        umbra_->Disconnect();
    }
}

void RGBController_Umbra::SetupZones()
{
    unsigned int start_idx = 0;

    for(const UmbraController::PortInfo& port : umbra_->GetPopulatedPorts())
    {
        zone new_zone;

        char zone_name[32];
        snprintf(zone_name, sizeof(zone_name), "ARGB Port %02u", port.port_index + 1u);

        new_zone.name        = zone_name;
        new_zone.type        = ZONE_TYPE_LINEAR;
        new_zone.flags       = 0;
        new_zone.start_idx   = start_idx;
        new_zone.leds_count  = port.led_count;
        new_zone.leds_min    = port.led_count;
        new_zone.leds_max    = port.led_count;
        new_zone.matrix_map  = nullptr;

        for(unsigned int led_idx = 0; led_idx < port.led_count; led_idx++)
        {
            char led_name[48];
            snprintf(led_name, sizeof(led_name), "%s LED %02u", zone_name, led_idx + 1u);

            led new_led;
            new_led.name  = led_name;
            new_led.value = start_idx + led_idx;

            leds.push_back(new_led);
        }

        zones.push_back(new_zone);

        start_idx += port.led_count;
    }

    SetupColors();
}

void RGBController_Umbra::ResizeZone(int /*zone*/, int /*new_size*/)
{
    /*-----------------------------------------------------*\
    | Zone sizes come from the hub topology and map 1:1 to  |
    | physical LEDs - resizing is not supported             |
    \*-----------------------------------------------------*/
}

bool RGBController_Umbra::SendFullFrame()
{
    if(umbra_ == nullptr || !umbra_->IsConnected())
    {
        return false;
    }

    /*-----------------------------------------------------*\
    | Pack OpenRGB colors into protocol byte order          |
    \*-----------------------------------------------------*/
    unsigned int total = umbra_->GetTotalLedCount();

    if(total == 0 || colors.size() < total)
    {
        return false;
    }

    std::vector<unsigned char> rgb;
    rgb.reserve((size_t)total * 3);

    for(unsigned int i = 0; i < total; i++)
    {
        RGBColor color = colors[i];

        rgb.push_back((unsigned char)RGBGetRValue(color));
        rgb.push_back((unsigned char)RGBGetGValue(color));
        rgb.push_back((unsigned char)RGBGetBValue(color));
    }

    return umbra_->SendFrame(rgb.data(), total);
}

void RGBController_Umbra::DeviceUpdateLEDs()
{
    SendFullFrame();
}

void RGBController_Umbra::UpdateZoneLEDs(int /*zone*/)
{
    /*-----------------------------------------------------*\
    | The protocol only supports whole-device frames        |
    \*-----------------------------------------------------*/
    SendFullFrame();
}

void RGBController_Umbra::UpdateSingleLED(int /*led*/)
{
    SendFullFrame();
}

void RGBController_Umbra::DeviceUpdateMode()
{
    /*-----------------------------------------------------*\
    | Only Direct mode exists; nothing to push to hardware  |
    \*-----------------------------------------------------*/
}

void RGBController_Umbra::DeviceSaveMode()
{
}
