#include "DisplayApp.h"
#include <FreeRTOS.h>
#include <task.h>
#include <libraries/log/nrf_log.h>
#include <boards.h>
#include <nrf_font.h>
#include <queue.h>
#include <Components/DateTime/DateTimeController.h>
#include <drivers/Cst816s.h>
#include <string>
#include <lvgl/lvgl.h>
#include <DisplayApp/Screens/Tile.h>
#include <DisplayApp/Screens/Message.h>
#include <DisplayApp/Screens/Meter.h>
#include <DisplayApp/Screens/Gauge.h>
#include "../SystemTask/SystemTask.h"

using namespace Pinetime::Applications;

DisplayApp::DisplayApp(Pinetime::Drivers::St7789& lcd,
                       Pinetime::Components::LittleVgl& lvgl,
                       Pinetime::Drivers::Cst816S& touchPanel,
                       Controllers::Battery &batteryController,
                       Controllers::Ble &bleController,
                       Controllers::DateTime &dateTimeController,
                       Pinetime::System::SystemTask& systemTask) :
        lcd{lcd},
        lvgl{lvgl},
        touchPanel{touchPanel},
        batteryController{batteryController},
        bleController{bleController},
        dateTimeController{dateTimeController},
        currentScreen{new Screens::Clock(this, dateTimeController, batteryController, bleController) },
        systemTask{systemTask} {
  msgQueue = xQueueCreate(queueSize, itemSize);
  onClockApp = true;
}

void DisplayApp::Start() {
  if (pdPASS != xTaskCreate(DisplayApp::Process, "DisplayApp", 512, this, 0, &taskHandle))
    APP_ERROR_HANDLER(NRF_ERROR_NO_MEM);
}

void DisplayApp::Process(void *instance) {
  auto *app = static_cast<DisplayApp *>(instance);
  NRF_LOG_INFO("DisplayApp task started!");
  app->InitHw();

  // Send a dummy notification to unlock the lvgl display driver for the first iteration
  xTaskNotifyGive(xTaskGetCurrentTaskHandle());

  while (1) {

    app->Refresh();

  }
}

void DisplayApp::InitHw() {
  nrf_gpio_cfg_output(pinLcdBacklight1);
  nrf_gpio_cfg_output(pinLcdBacklight2);
  nrf_gpio_cfg_output(pinLcdBacklight3);
  nrf_gpio_pin_clear(pinLcdBacklight1);
  nrf_gpio_pin_clear(pinLcdBacklight2);
  nrf_gpio_pin_clear(pinLcdBacklight3);
}

uint32_t acc = 0;
uint32_t count = 0;
bool toggle = true;
void DisplayApp::Refresh() {
  TickType_t queueTimeout;
  switch (state) {
    case States::Idle:
      IdleState();
      queueTimeout = portMAX_DELAY;
      break;
    case States::Running:
      RunningState();
      queueTimeout = 20;
      break;
  }

  Messages msg;
  if (xQueueReceive(msgQueue, &msg, queueTimeout)) {
    switch (msg) {
      case Messages::GoToSleep:
        nrf_gpio_pin_set(pinLcdBacklight3);
        vTaskDelay(100);
        nrf_gpio_pin_set(pinLcdBacklight2);
        vTaskDelay(100);
        nrf_gpio_pin_set(pinLcdBacklight1);
        lcd.DisplayOff();
        lcd.Sleep();
        touchPanel.Sleep();
        state = States::Idle;
        break;
      case Messages::GoToRunning:
        lcd.Wakeup();
        touchPanel.Wakeup();

        lcd.DisplayOn();
        nrf_gpio_pin_clear(pinLcdBacklight3);
        nrf_gpio_pin_clear(pinLcdBacklight2);
        nrf_gpio_pin_clear(pinLcdBacklight1);
        state = States::Running;
        break;
      case Messages::UpdateDateTime:
        break;
      case Messages::UpdateBleConnection:
//        clockScreen.SetBleConnectionState(bleController.IsConnected() ? Screens::Clock::BleConnectionStates::Connected : Screens::Clock::BleConnectionStates::NotConnected);
        break;
      case Messages::UpdateBatteryLevel:
//        clockScreen.SetBatteryPercentRemaining(batteryController.PercentRemaining());
        break;
      case Messages::TouchEvent: {
        if (state != States::Running) break;
        auto gesture = OnTouchEvent();
        switch (gesture) {
          case DisplayApp::TouchEvents::SwipeUp:
            currentScreen->OnButtonPushed();
            lvgl.SetFullRefresh(Components::LittleVgl::FullRefreshDirections::Up);
            break;
          case DisplayApp::TouchEvents::SwipeDown:
            currentScreen->OnButtonPushed();
            lvgl.SetFullRefresh(Components::LittleVgl::FullRefreshDirections::Down);
            break;
          default:
            break;
        }
      }
        break;
      case Messages::ButtonPushed:
        if(onClockApp)
            systemTask.PushMessage(System::SystemTask::Messages::GoToSleep);
          else {
            auto buttonUsedByApp = currentScreen->OnButtonPushed();
            if (!buttonUsedByApp) {
              systemTask.PushMessage(System::SystemTask::Messages::GoToSleep);
            } else {
              lvgl.SetFullRefresh(Components::LittleVgl::FullRefreshDirections::Up);
          }
        }

//        lvgl.SetFullRefresh(Components::LittleVgl::FullRefreshDirections::Down);
//        currentScreen.reset(nullptr);
//        if(toggle) {
//          currentScreen.reset(new Screens::Tile(this));
//          toggle = false;
//        } else {
//          currentScreen.reset(new Screens::Clock(this, dateTimeController, batteryController, bleController));
//          toggle = true;
//        }

        break;
    }
  }
}

void DisplayApp::RunningState() {
//  clockScreen.SetCurrentDateTime(dateTimeController.CurrentDateTime());

  if(!currentScreen->Refresh()) {
    currentScreen.reset(nullptr);
    lvgl.SetFullRefresh(Components::LittleVgl::FullRefreshDirections::Up);
    onClockApp = false;
    switch(nextApp) {
      case Apps::None:
      case Apps::Launcher: currentScreen.reset(new Screens::Tile(this)); break;
      case Apps::Clock:
        currentScreen.reset(new Screens::Clock(this, dateTimeController, batteryController, bleController));
        onClockApp = true;
        break;
      case Apps::Test: currentScreen.reset(new Screens::Message(this)); break;
      case Apps::Meter: currentScreen.reset(new Screens::Meter(this)); break;
      case Apps::Gauge: currentScreen.reset(new Screens::Gauge(this)); break;
    }
    nextApp = Apps::None;
  }
  lv_task_handler();
}

void DisplayApp::IdleState() {

}

void DisplayApp::PushMessage(DisplayApp::Messages msg) {
  BaseType_t xHigherPriorityTaskWoken;
  xHigherPriorityTaskWoken = pdFALSE;
  xQueueSendFromISR(msgQueue, &msg, &xHigherPriorityTaskWoken);
  if (xHigherPriorityTaskWoken) {
    /* Actual macro used here is port specific. */
    // TODO : should I do something here?
  }
}

DisplayApp::TouchEvents DisplayApp::OnTouchEvent() {
  auto info = touchPanel.GetTouchInfo();
  if(info.isTouch) {
    switch(info.gesture) {
      case Pinetime::Drivers::Cst816S::Gestures::SingleTap:
        // TODO set x,y to LittleVgl
        lvgl.SetNewTapEvent(info.x, info.y);
        return DisplayApp::TouchEvents::Tap;
      case Pinetime::Drivers::Cst816S::Gestures::LongPress:
        return DisplayApp::TouchEvents::LongTap;
      case Pinetime::Drivers::Cst816S::Gestures::DoubleTap:
        return DisplayApp::TouchEvents::DoubleTap;
      case Pinetime::Drivers::Cst816S::Gestures::SlideRight:
        return DisplayApp::TouchEvents::SwipeRight;
      case Pinetime::Drivers::Cst816S::Gestures::SlideLeft:
        return DisplayApp::TouchEvents::SwipeLeft;
      case Pinetime::Drivers::Cst816S::Gestures::SlideDown:
        return DisplayApp::TouchEvents::SwipeDown;
      case Pinetime::Drivers::Cst816S::Gestures::SlideUp:
        return DisplayApp::TouchEvents::SwipeUp;
      case Pinetime::Drivers::Cst816S::Gestures::None:
      default:
        return DisplayApp::TouchEvents::None;
    }
  }
  return DisplayApp::TouchEvents::None;
}

void DisplayApp::StartApp(DisplayApp::Apps app) {
  nextApp = app;
}
