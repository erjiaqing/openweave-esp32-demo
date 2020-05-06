/*
 *
 *    Copyright (c) 2018 Nest Labs, Inc.
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/*
 *   Description:
 *     OpenWeave ESP32 demo application.
 */

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_heap_caps_init.h"
#include <new>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <Weave/DeviceLayer/WeaveDeviceLayer.h>
#include <Weave/Support/ErrorStr.h>

#include <openthread/openthread-system.h>
#include <openthread/cli.h>

#include "AliveTimer.h"
#include "ServiceEcho.h"
#include "Display.h"
#include "TitleWidget.h"
#include "StatusIndicatorWidget.h"
#include "PairingWidget.h"
#include "CountdownWidget.h"
#include "LEDWidget.h"
#include "Button.h"
#include "LightController.h"
#include "LightSwitch.h"

using namespace ::nl;
using namespace ::nl::Inet;
using namespace ::nl::Weave;
using namespace ::nl::Weave::DeviceLayer;

const char * TAG = "openweave-demo";

#if CONFIG_DEVICE_TYPE_M5STACK

#define ATTENTION_BUTTON_GPIO_NUM GPIO_NUM_37               // Use the right button (button "C") as the attention button on M5Stack
#define STATUS_LED_GPIO_NUM GPIO_NUM_MAX                    // No status LED on M5Stack
#define LIGHT_SWITCH_ON_BUTTON_GPIO_NUM GPIO_NUM_39         // Use the left button (button "A") as the light switch ON button on M5Stack
#define LIGHT_SWITCH_OFF_BUTTON_GPIO_NUM GPIO_NUM_38        // Use the middle button (button "B") as the light switch OFF button on M5Stack
#define LIGHT_CONTROLLER_OUTPUT_GPIO_NUM GPIO_NUM_2         // Use GPIO2 as the light controller output on M5Stack

#elif CONFIG_DEVICE_TYPE_ESP32_DEVKITC

#define ATTENTION_BUTTON_GPIO_NUM GPIO_NUM_0                // Use the IO0 button as the attention button on ESP32-DevKitC and compatibles
#define STATUS_LED_GPIO_NUM GPIO_NUM_2                      // Use LED1 (blue LED) as status LED on DevKitC
#define LIGHT_SWITCH_ON_BUTTON_GPIO_NUM GPIO_NUM_34         // Use GPIO34 as the light switch ON button input on DevKitC
#define LIGHT_SWITCH_OFF_BUTTON_GPIO_NUM GPIO_NUM_35        // Use GPIO35 as the light switch OFF button input on DevKitC
#define LIGHT_CONTROLLER_OUTPUT_GPIO_NUM GPIO_NUM_33        // Use GPIO33 as the light controller output on DevKitC

#else // !CONFIG_DEVICE_TYPE_ESP32_DEVKITC

#error "Unsupported device type selected"

#endif // !CONFIG_DEVICE_TYPE_ESP32_DEVKITC

namespace nl {
namespace Weave {
namespace Profiles {
namespace DataManagement_Current {
namespace Platform {

void CriticalSectionEnter(void)
{
    return ;
}

void CriticalSectionExit(void)
{
    return ;
}

} // namespace Platform
} // namespace DataManagement_Current
} // namespace Profiles
} // namespace Weave
} // namespace nl

#if CONFIG_HAVE_DISPLAY

enum DisplayMode
{
    kDisplayMode_Uninitialized,
    kDisplayMode_StatusScreen,
    kDisplayMode_PairingScreen,
    kDisplayMode_ResetCountdown,
};

static DisplayMode displayMode = kDisplayMode_Uninitialized;
static TitleWidget titleWidget;
static StatusIndicatorWidget statusIndicator;
static PairingWidget pairingWidget;
static CountdownWidget resetCountdownWidget;

static const char *resetMsg = "Reset to defaults in";

#endif // CONFIG_HAVE_DISPLAY

static Button attentionButton;
static LEDWidget statusLED;
static bool isWiFiStationProvisioned = false;
static bool isWiFiStationEnabled = false;
static bool isWiFiStationConnected = false;
static bool isWiFiAPActive = false;
static bool haveIPv4Connectivity = false;
static bool isServiceProvisioned = false;
static bool haveServiceConnectivity = false;
static bool haveBLEConnections = false;
static bool isServiceSubscriptionEstablished = false;
static bool isPairedToAccount = true;
static volatile bool commissionerDetected = false;

#if CONFIG_ENABLE_LIGHTING_DEMO_FEATURE

static Button lightSwitchOnButton;
static Button lightSwitchOffButton;
static LightController lightController;
static LightSwitch lightSwitch;
static bool isLightingController;

static enum { None, Up, Down } curDimAction = None;
static uint8_t dimStartLevel = 0;

#define DIM_START_DELAY 500u        // Amount of time user must hold a button to start the dimming action (in ms)
#define MAX_DIM_TIME 4000u          // The maximum amount of time required to dim across the entire brightness range (in ms)
#define DIM_UPDATE_INTERVAL 100u    // The minimum amount of time between commands sent to the light controller while dimming (in ms)

#define TOTAL_DIM_UPDATE_INTERVALS (MAX_DIM_TIME / DIM_UPDATE_INTERVAL)

#endif // CONFIG_ENABLE_LIGHTING_DEMO_FEATURE

static void DeviceEventHandler(const WeaveDeviceEvent * event, intptr_t arg);

extern "C" void thread_init_task(void * arg)
{
    TaskHandle_t *called_task = (TaskHandle_t *) arg;
    otInstance *instance;
    WEAVE_ERROR err;
    
    ESP_LOGE(TAG, "otSysInit()");
    otSysInit(0, NULL);
    ESP_LOGE(TAG, "otSysInit() Done");
    size_t instanceSize = 0;
    // Get the instance size.
    otInstanceInit(NULL, &instanceSize);
    void *instanceBuffer = malloc(instanceSize);

    instance = otInstanceInit(instanceBuffer, &instanceSize);
    assert(instance != NULL);
 
    otCliUartInit(instance);

    ESP_LOGI(TAG, "Init OpenThread Stack");
    err = ThreadStackMgrImpl().InitThreadStack(instance);
    if (err != WEAVE_NO_ERROR) {
        ESP_LOGE(TAG, "ThreadStackMgr().InitThreadStack() failed: %s", ErrorStr(err));
        return;
    }

    ESP_LOGI(TAG, "Starting OpenThread Task");
    err = ThreadStackMgr().StartThreadTask();
    if (err != WEAVE_NO_ERROR) {
        ESP_LOGE(TAG, "ThreadStackMgr().StartThreadStack() failed: %s", ErrorStr(err));
        return;
    }

    // Notify main task to continue
    xTaskNotify(*called_task, ( 1UL ), eSetBits );
    vTaskDelete(NULL);
}

void ot_init()
{
    TaskHandle_t xHandle = NULL;
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    BaseType_t res;
    res = xTaskCreate(thread_init_task,
                "otinit",
                13720 / sizeof(StackType_t),
                &current_task,
                5,
                &xHandle);
    assert(res == pdPASS);
    
    // Wait until thread_init_task finish
    while (true)
    {
        uint32_t ulNotifiedValue;
        xTaskNotifyWait(0x00, ULONG_MAX, &ulNotifiedValue, portMAX_DELAY);
        if (ulNotifiedValue & 1)
        {
            break;
        }
    }
}

extern "C" void app_main()
{
    WEAVE_ERROR err;    // A quick note about errors: Weave adopts the error type and numbering
                        // convention of the environment into which it is ported.  Thus esp_err_t
                        // and WEAVE_ERROR are in fact the same type, and both ESP-IDF errors
                        // and Weave-specific errors can be stored in the same value without
                        // ambiguity.  For convenience, ESP_OK and WEAVE_NO_ERROR are mapped
                        // to the same value.

    // Initialize the ESP NVS layer.
    err = nvs_flash_init();
    if (err != WEAVE_NO_ERROR)
    {
        ESP_LOGE(TAG, "nvs_flash_init() failed: %s", ErrorStr(err));
        return;
    }

    // Initialize the LwIP core lock.  This must be done before the ESP
    // tcpip_adapter layer is initialized.
    err = PlatformMgrImpl().InitLwIPCoreLock();
    if (err != WEAVE_NO_ERROR)
    {
        ESP_LOGE(TAG, "PlatformMgr().InitLocks() failed: %s", ErrorStr(err));
        return;
    }

    // Initialize the ESP tcpip adapter.
    tcpip_adapter_init();

    // Arrange for the ESP event loop to deliver events into the Weave Device layer.
    err = esp_event_loop_init(PlatformManagerImpl::HandleESPSystemEvent, NULL);
    if (err != WEAVE_NO_ERROR)
    {
        ESP_LOGE(TAG, "esp_event_loop_init() failed: %s", ErrorStr(err));
        return;
    }

    // Initialize the ESP WiFi layer.
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != WEAVE_NO_ERROR)
    {
        ESP_LOGE(TAG, "esp_event_loop_init() failed: %s", ErrorStr(err));
        return;
    }

    // Initialize the Weave stack.
    err = PlatformMgr().InitWeaveStack();
    if (err != WEAVE_NO_ERROR)
    {
        ESP_LOGE(TAG, "PlatformMgr().InitWeaveStack() failed: %s", ErrorStr(err));
        return;
    }

    // OpenThread requires more stacks, create another task to run it.
    ot_init();

    // Configure the Weave Connectivity Manager to automatically enable the WiFi AP interface
    // whenever the WiFi station interface has not be configured.
    ConnectivityMgr().SetWiFiAPMode(ConnectivityManager::kWiFiAPMode_OnDemand_NoStationProvision);

    // Register a function to receive events from the Weave device layer.  Note that calls to
    // this function will happen on the Weave event loop thread, not the app_main thread.
    PlatformMgr().AddEventHandler(DeviceEventHandler, 0);

#if CONFIG_ALIVE_INTERVAL
    // Start a Weave-based timer that will print an 'Alive' message on a periodic basis.  This
    // confirms that the Weave thread is alive and processing events.
    err = StartAliveTimer(CONFIG_ALIVE_INTERVAL);
    if (err != WEAVE_NO_ERROR)
    {
        return;
    }
#endif // CONFIG_ALIVE_INTERVAL

#if CONFIG_SERVICE_ECHO_INTERVAL
    // Start a Weave echo client that will periodically send Weave Echo requests to the Nest service
    // whenever the service tunnel is established.
    err = ServiceEcho.Init(CONFIG_SERVICE_ECHO_INTERVAL);
    if (err != WEAVE_NO_ERROR)
    {
        return;
    }
#endif // CONFIG_SERVICE_ECHO_INTERVAL

    // Initialize the attention button.
    err = attentionButton.Init(ATTENTION_BUTTON_GPIO_NUM, 50);
    if (err != WEAVE_NO_ERROR)
    {
        ESP_LOGE(TAG, "Button.Init() failed: %s", ErrorStr(err));
        return;
    }

    // Initialize the status LED.
    statusLED.Init(STATUS_LED_GPIO_NUM);

#if CONFIG_ENABLE_LIGHTING_DEMO_FEATURE

    // Determine if we're acting as a lighting controller or switch
    isLightingController = (::nl::Weave::DeviceLayer::FabricState.LocalNodeId == CONFIG_LIGHTING_CONTROLLER_DEVICE_ID);

    if (isLightingController)
    {
        // Initialize the light controller object.
        err = lightController.Init(LIGHT_CONTROLLER_OUTPUT_GPIO_NUM);
        if (err != WEAVE_NO_ERROR)
        {
            ESP_LOGE(TAG, "LightContoller.Init() failed: %s", nl::ErrorStr(err));
            return;
        }

        ESP_LOGI(TAG, "Lighting demo feature enabled: Serving as Light Controller");
    }

    else
    {
        // Initialize the remote light switch object.
        err = lightSwitch.Init(CONFIG_LIGHTING_CONTROLLER_DEVICE_ID);
        if (err != WEAVE_NO_ERROR)
        {
            ESP_LOGE(TAG, "LightSwitch.Init() failed: %s", nl::ErrorStr(err));
            return;
        }

        // Initialize the light switch ON button.
        err = lightSwitchOnButton.Init(LIGHT_SWITCH_ON_BUTTON_GPIO_NUM, 50);
        if (err != WEAVE_NO_ERROR)
        {
            ESP_LOGE(TAG, "Button.Init() failed: %s", ErrorStr(err));
            return;
        }

        // Initialize the light switch OFF button.
        err = lightSwitchOffButton.Init(LIGHT_SWITCH_OFF_BUTTON_GPIO_NUM, 50);
        if (err != WEAVE_NO_ERROR)
        {
            ESP_LOGE(TAG, "Button.Init() failed: %s", ErrorStr(err));
            return;
        }

        ESP_LOGI(TAG, "Lighting demo feature enabled: Serving as Light Switch for controller at %016" PRIX64,
                 CONFIG_LIGHTING_CONTROLLER_DEVICE_ID);
    }

#endif // CONFIG_ENABLE_LIGHTING_DEMO_FEATURE

#if CONFIG_HAVE_DISPLAY

    // Initialize the display device.
    err = InitDisplay();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "InitDisplay() failed: %s", ErrorStr(err));
        return;
    }

    // Initialize the UI widgets.
    titleWidget.Init("Blue Sky");
    pairingWidget.Init();
    statusIndicator.Init(5);
    statusIndicator.Char[0] = 'W';
    statusIndicator.Char[1] = 'I';
    statusIndicator.Char[2] = 'T';
    statusIndicator.Char[3] = 'S';
    statusIndicator.Char[4] = 'A';
    resetCountdownWidget.Init(3);

#endif // CONFIG_HAVE_DISPLAY

    ESP_LOGI(TAG, "Ready");

#if CONFIG_HAVE_DISPLAY

    // If the device has a display, run the title animation before starting the Weave stack.
    titleWidget.Start();
    while (true)
    {
        titleWidget.Animate();
        if (titleWidget.Done)
        {
            break;
        }
        vTaskDelay(50 / portTICK_RATE_MS);
    }

    // Display the status indicators.
    statusIndicator.Display();
    displayMode = kDisplayMode_StatusScreen;

#endif // CONFIG_HAVE_DISPLAY

    // Start a task to run the Weave Device event loop.
    err = PlatformMgr().StartEventLoopTask();
    if (err != WEAVE_NO_ERROR)
    {
        ESP_LOGE(TAG, "PlatformMgr().StartEventLoopTask() failed: %s", ErrorStr(err));
        return;
    }

    // Repeatedly loop to drive the UI...
    while (true)
    {
        // Collect connectivity and configuration state from the Weave stack.  Because the
        // Weave event loop is being run in a separate task, the stack must be locked
        // while these values are queried.  However we use a non-blocking lock request
        // (TryLockWeaveStack()) to avoid blocking other UI activities when the Weave
        // task is busy (e.g. with a long crypto operation).
        if (PlatformMgr().TryLockWeaveStack())
        {
            isWiFiStationProvisioned = ConnectivityMgr().IsWiFiStationProvisioned();
            isWiFiStationEnabled = ConnectivityMgr().IsWiFiStationEnabled();
            isWiFiStationConnected = ConnectivityMgr().IsWiFiStationConnected();
            isWiFiAPActive = ConnectivityMgr().IsWiFiAPActive();
            haveBLEConnections = (ConnectivityMgr().NumBLEConnections() != 0);
            haveIPv4Connectivity = ConnectivityMgr().HaveIPv4InternetConnectivity();
            isServiceProvisioned = ConfigurationMgr().IsServiceProvisioned();
            isPairedToAccount = ConfigurationMgr().IsPairedToAccount();
            haveServiceConnectivity = ConnectivityMgr().HaveServiceConnectivity();
            isServiceSubscriptionEstablished = TraitMgr().IsServiceSubscriptionEstablished();

            PlatformMgr().UnlockWeaveStack();
        }

        // Consider the system to be "fully connected" if it has IPv4 connectivity, service
        // connectivity and it is able to interact with the service on a regular basis.
        bool isFullyConnected = (haveIPv4Connectivity && haveServiceConnectivity && isServiceSubscriptionEstablished);

        // Update the status LED...
        //
        // If the WiFi station interface is provisioned and enabled, but the system doesn't have
        // WiFi/Internet connectivity, OR if the system is service provisioned, but not yet able
        // to talk to the service, THEN blink the LED at an even 500ms rate to signal the system
        // is in the process of establishing connectivity.
        //
        // If the system is "fully connected" then turn the LED on, UNLESS the AP is enabled, or
        // there is a BLE connection, in which case blink the LED off for a short period of time.
        //
        // Finally, if the system is not "fully connected" then turn the LED off, UNLESS the AP is
        // enabled, or there is a BLE connection, in which case blink the LED on for a short period
        // of time.
        //
        if ((isWiFiStationProvisioned && isWiFiStationEnabled && (!isWiFiStationConnected || !haveIPv4Connectivity)) ||
            (isServiceProvisioned && (!haveServiceConnectivity || !isServiceSubscriptionEstablished)))
        {
            statusLED.Blink(500);
        }
        else if (isFullyConnected)
        {
            if (isWiFiAPActive || haveBLEConnections)
            {
                statusLED.Blink(950, 50);
            }
            else
            {
                statusLED.Set(true);
            }
        }
        else
        {
            if (isWiFiAPActive || haveBLEConnections)
            {
                statusLED.Blink(50, 950);
            }
            else
            {
                statusLED.Set(true);
            }
        }
        statusLED.Animate();

        // Poll the attention button.  Whenever we detect a *release* of the button
        // demand start the WiFi AP interface and place the device in "user selected"
        // mode.
        //
        // While the device is in user selected mode, it will respond to Device
        // Identify Requests that have the UserSelectedMode flag set.  This makes it
        // easy for other mobile applications or devices to find it.
        //
        bool attentionButtonPressDetected = false;
        (void)attentionButtonPressDetected;
        if (attentionButton.Poll() && !attentionButton.IsPressed())
        {
            PlatformMgr().LockWeaveStack();
            ConnectivityMgr().DemandStartWiFiAP();
            ConnectivityMgr().SetUserSelectedMode(true);
            PlatformMgr().UnlockWeaveStack();
            attentionButtonPressDetected = true;
        }

        // If the attention button has been pressed for more that the factory reset
        // press duration, initiate a factory reset of the device.
        if (attentionButton.IsPressed() &&
            attentionButton.GetStateDuration() > CONFIG_FACTORY_RESET_BUTTON_DURATION)
        {
#if CONFIG_HAVE_DISPLAY
            ClearDisplay();
#endif
            PlatformMgr().LockWeaveStack();
            ConfigurationMgr().InitiateFactoryReset();
            PlatformMgr().UnlockWeaveStack();
            return;
        }

#if CONFIG_ENABLE_LIGHTING_DEMO_FEATURE

        // If acting as a remote light switch...
        if (!isLightingController)
        {
            // Poll the state of the light switch buttons.
            bool onButtonChange = lightSwitchOnButton.Poll();
            bool offButtonChange = lightSwitchOffButton.Poll();

            PlatformMgr().LockWeaveStack();

            // Prepare new state and level values for the light.
            uint8_t lightState = lightSwitch.GetState();
            uint8_t lightLevel = lightSwitch.GetLevel();

            if (onButtonChange)
            {
                if (lightSwitchOnButton.IsPressed())
                {
                    lightState = LightSwitch::ON;
                    curDimAction = Up;
                    dimStartLevel = lightSwitch.GetLevel();
                }
                else if (curDimAction == Up)
                {
                    curDimAction = None;
                }
            }
            else if (offButtonChange)
            {
                if (lightSwitchOffButton.IsPressed())
                {
                    curDimAction = Down;
                    dimStartLevel = lightSwitch.GetLevel();
                }
                else
                {
                    if (curDimAction == Down)
                    {
                        curDimAction = None;
                    }
                    if (lightSwitchOffButton.GetPrevStateDuration() <= DIM_START_DELAY)
                    {
                        lightState = LightSwitch::OFF;
                    }
                }
            }

            if (curDimAction != None)
            {
                uint32_t switchStateDur = (curDimAction == Up)
                        ? lightSwitchOnButton.GetStateDuration()
                        : lightSwitchOffButton.GetStateDuration();

                if (switchStateDur > DIM_START_DELAY)
                {
                    uint32_t elapsedUpdateIntervals = ((switchStateDur - DIM_START_DELAY) / DIM_UPDATE_INTERVAL) + 1;

                    uint32_t levelDelta = (elapsedUpdateIntervals * 100) / TOTAL_DIM_UPDATE_INTERVALS;
                    if (levelDelta > 100)
                    {
                        levelDelta = 100;
                    }

                    if (curDimAction == Up)
                    {
                        lightLevel = ((100 - levelDelta) > dimStartLevel) ? dimStartLevel + levelDelta : 100;
                    }
                    else
                    {
                        lightLevel = (levelDelta < dimStartLevel) ? dimStartLevel - levelDelta : 0;
                    }
                }
            }

            if (lightState != lightSwitch.GetState() || lightLevel != lightSwitch.GetLevel())
            {
                lightSwitch.Set(lightState, lightLevel);
            }

            PlatformMgr().UnlockWeaveStack();
        }

#endif // CONFIG_ENABLE_LIGHTING_DEMO_FEATURE

#if CONFIG_HAVE_DISPLAY

        // Update the status indicators.
        statusIndicator.State[0] = isWiFiStationConnected;
        statusIndicator.State[1] = haveIPv4Connectivity;
        statusIndicator.State[2] = haveServiceConnectivity;
        statusIndicator.State[3] = isServiceSubscriptionEstablished;
        statusIndicator.State[4] = (haveBLEConnections || isWiFiAPActive);
        statusIndicator.Char[4] = (haveBLEConnections) ? 'B' : 'A';

        // If NOT currently showing the reset countdown screen, and the attention button
        // has been pressed long enough that the reset time is less than the total
        // countdown time away, then switch to the countdown screen.
        if (displayMode != kDisplayMode_ResetCountdown)
        {
            if (attentionButton.IsPressed() &&
                attentionButton.GetStateDuration() + resetCountdownWidget.TotalDurationMS() >= CONFIG_FACTORY_RESET_BUTTON_DURATION)
            {
                ClearDisplay();
                TFT_setFont(DEJAVU24_FONT, NULL);
                _fg = titleWidget.TitleColor;
                _bg = TFT_BLACK;
                DisplayMessageCentered(resetMsg, 25);
                resetCountdownWidget.Start();
                displayMode = kDisplayMode_ResetCountdown;
            }
        }

        // If currently displaying the status screen and the attention button
        // is pressed while the device is not paired to an account, switch to
        // the pairing screen.
        if (displayMode == kDisplayMode_StatusScreen)
        {
            if (!isPairedToAccount && attentionButtonPressDetected)
            {
                ClearDisplay();
                pairingWidget.Display();
                commissionerDetected = false;
                displayMode = kDisplayMode_PairingScreen;
            }
        }

        // If currently displaying the pairing screen and...
        //     the WiFI AP is no longer active,
        //     OR the device is now paired to an account,
        //     OR a session from a commissioner application has been established,
        //     OR the attention button is pressed
        // then switch back to the status screen.
        else if (displayMode == kDisplayMode_PairingScreen)
        {
            if (!isWiFiAPActive || isPairedToAccount ||
                commissionerDetected || attentionButtonPressDetected)
            {
                ClearDisplay();
                titleWidget.Start();
                statusIndicator.Display();
                displayMode = kDisplayMode_StatusScreen;
            }
        }

        // If currently displaying the reset countdown screen and the attention
        // button is released, switch back to the status screen.
        else if (displayMode == kDisplayMode_ResetCountdown)
        {
            if (!attentionButton.IsPressed())
            {
                ClearDisplay();
                titleWidget.Start();
                statusIndicator.Display();
                displayMode = kDisplayMode_StatusScreen;
            }
        }

        // If displaying the status screen, run the title animation and update
        // the status indicators.
        if (displayMode == kDisplayMode_StatusScreen)
        {
            titleWidget.Animate();
            statusIndicator.Update();
        }

        // If displaying the reset countdown screen, update the countdown indicators.
        else if (displayMode == kDisplayMode_ResetCountdown)
        {
            resetCountdownWidget.Update();
        }

#endif // CONFIG_HAVE_DISPLAY

        vTaskDelay(50 / portTICK_RATE_MS);
    }
}

/* Handle events from the Weave Device layer.
 *
 * NOTE: This function runs on the Weave event loop task.
 */
void DeviceEventHandler(const WeaveDeviceEvent * event, intptr_t arg)
{
    if (event->Type == DeviceEventType::kSessionEstablished &&
        event->SessionEstablished.IsCommissioner)
    {
        commissionerDetected = true;
    }
}
