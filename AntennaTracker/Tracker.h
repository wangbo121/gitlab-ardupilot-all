/// -*- tab-width: 4; Mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-

#define THISFIRMWARE "AntennaTracker V0.7.2"
/*
   Lead developers: Matthew Ridley and Andrew Tridgell
 
   Please contribute your ideas! See http://dev.ardupilot.com for details

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

////////////////////////////////////////////////////////////////////////////////
// Header includes
////////////////////////////////////////////////////////////////////////////////

#include <math.h>
#include <stdarg.h>
#include <stdio.h>

#include <AP_Common.h>
#include <AP_Progmem.h>
#include <AP_HAL.h>
#include <AP_Param.h>
#include <StorageManager.h>
#include <AP_GPS.h>         // ArduPilot GPS library
#include <AP_Baro.h>        // ArduPilot barometer library
#include <AP_Compass.h>     // ArduPilot Mega Magnetometer Library
#include <AP_Math.h>        // ArduPilot Mega Vector/Matrix math Library
#include <AP_ADC.h>         // ArduPilot Mega Analog to Digital Converter Library
#include <AP_ADC_AnalogSource.h>
#include <AP_InertialSensor.h> // Inertial Sensor Library
#include <AP_AHRS.h>         // ArduPilot Mega DCM Library
#include <Filter.h>                     // Filter library
#include <AP_Buffer.h>      // APM FIFO Buffer
#include <memcheck.h>

#include <GCS_MAVLink.h>    // MAVLink GCS definitions
#include <AP_SerialManager.h>   // Serial manager library
#include <AP_Declination.h> // ArduPilot Mega Declination Helper Library
#include <DataFlash.h>
#include <SITL.h>
#include <PID.h>
#include <AP_Scheduler.h>       // main loop scheduler
#include <AP_NavEKF.h>

#include <AP_Vehicle.h>
#include <AP_Mission.h>
#include <AP_Terrain.h>
#include <AP_Rally.h>
#include <AP_Notify.h>      // Notify library
#include <AP_BattMonitor.h> // Battery monitor library
#include <AP_Airspeed.h>
#include <RC_Channel.h>
#include <AP_BoardConfig.h>
#include <AP_OpticalFlow.h>
#include <AP_RangeFinder.h>

// Configuration
#include "config.h"
#include "defines.h"

#include "Parameters.h"
#include "GCS.h"

#include <AP_HAL_AVR.h>
#include <AP_HAL_SITL.h>
#include <AP_HAL_PX4.h>
#include <AP_HAL_FLYMAPLE.h>
#include <AP_HAL_Linux.h>
#include <AP_HAL_Empty.h>

class Tracker {
public:
    friend class GCS_MAVLINK;
    friend class Parameters;

    Tracker(void);
    void setup();
    void loop();

private:
    const AP_InertialSensor::Sample_rate ins_sample_rate = AP_InertialSensor::RATE_50HZ;

    Parameters g;

    // main loop scheduler
    AP_Scheduler scheduler;
 
    // notification object for LEDs, buzzers etc
    AP_Notify notify;

    uint32_t start_time_ms = 0;

    bool usb_connected = false;

    AP_GPS gps;

    AP_Baro barometer;

    Compass compass;

    AP_InertialSensor ins;

    RangeFinder rng;

// Inertial Navigation EKF
#if AP_AHRS_NAVEKF_AVAILABLE
    NavEKF EKF{&ahrs, barometer, rng};
    AP_AHRS_NavEKF ahrs{ins, barometer, gps, rng, EKF};
#else
    AP_AHRS_DCM ahrs{ins, barometer, gps};
#endif

#if CONFIG_HAL_BOARD == HAL_BOARD_SITL
    SITL sitl;
#endif
    
    /**
       antenna control channels
    */
    RC_Channel channel_yaw{CH_YAW};
    RC_Channel channel_pitch{CH_PITCH};

    AP_SerialManager serial_manager;
    const uint8_t num_gcs = MAVLINK_COMM_NUM_BUFFERS;
    GCS_MAVLINK gcs[MAVLINK_COMM_NUM_BUFFERS];

    AP_BoardConfig BoardConfig;

    struct Location current_loc;

    enum ControlMode control_mode  = INITIALISING;

    // Vehicle state
    struct {
        bool location_valid;    // true if we have a valid location for the vehicle
        Location location;      // lat, long in degrees * 10^7; alt in meters * 100
        Location location_estimate; // lat, long in degrees * 10^7; alt in meters * 100
        uint32_t last_update_us;    // last position update in micxroseconds
        uint32_t last_update_ms;    // last position update in milliseconds
        float heading;          // last known direction vehicle is moving
        float ground_speed;     // vehicle's last known ground speed in m/s
    } vehicle;

    // Navigation controller state
    struct {
        float bearing;                  // bearing to vehicle in centi-degrees
        float distance;                 // distance to vehicle in meters
        float pitch;                    // pitch to vehicle in degrees (positive means vehicle is above tracker, negative means below)
        float altitude_difference;      // altitude difference between tracker and vehicle in meters.  positive value means vehicle is above tracker
        float altitude_offset;          // offset in meters which is added to tracker altitude to align altitude measurements with vehicle's barometer
        bool manual_control_yaw         : 1;// true if tracker yaw is under manual control
        bool manual_control_pitch       : 1;// true if tracker pitch is manually controlled
        bool need_altitude_calibration  : 1;// true if tracker altitude has not been determined (true after startup)
        bool scan_reverse_pitch         : 1;// controls direction of pitch movement in SCAN mode
        bool scan_reverse_yaw           : 1;// controls direction of yaw movement in SCAN mode
    } nav_status = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false, false, true, false, false};

    // Servo state
    struct {
        bool yaw_lower      : 1;    // true if yaw servo has been limited from moving to a lower position (i.e. position or rate limited)
        bool yaw_upper      : 1;    // true if yaw servo has been limited from moving to a higher position (i.e. position or rate limited)
        bool pitch_lower    : 1;    // true if pitch servo has been limited from moving to a lower position (i.e. position or rate limited)
        bool pitch_upper    : 1;    // true if pitch servo has been limited from moving to a higher position (i.e. position or rate limited)
    } servo_limit = {true, true, true, true};

    // setup the var_info table
    AP_Param param_loader{var_info};

    uint8_t one_second_counter = 0;
    bool target_set = false;
    int8_t slew_dir = 0;
    uint32_t slew_start_ms = 0;

    static const AP_Scheduler::Task scheduler_tasks[];
    static const AP_Param::Info var_info[];

    void one_second_loop();
    void send_heartbeat(mavlink_channel_t chan);
    void send_attitude(mavlink_channel_t chan);
    void send_location(mavlink_channel_t chan);
    void send_radio_out(mavlink_channel_t chan);
    void send_hwstatus(mavlink_channel_t chan);
    void send_waypoint_request(mavlink_channel_t chan);
    void send_statustext(mavlink_channel_t chan);
    void send_nav_controller_output(mavlink_channel_t chan);
    void send_simstate(mavlink_channel_t chan);
    void mavlink_check_target(const mavlink_message_t* msg);
    void gcs_send_message(enum ap_message id);
    void gcs_data_stream_send(void);
    void gcs_update(void);
    void gcs_send_text_P(gcs_severity severity, const prog_char_t *str);
    void gcs_retry_deferred(void);
    void load_parameters(void);
    void update_auto(void);
    void update_manual(void);
    void update_scan(void);
    bool servo_test_set_servo(uint8_t servo_num, uint16_t pwm);
    void read_radio();
    void init_barometer(void);
    void update_barometer(void);
    void update_ahrs();
    void update_compass(void);
    void compass_accumulate(void);
    void barometer_accumulate(void);
    void update_GPS(void);
    void init_servos();
    void update_pitch_servo(float pitch);
    void update_pitch_position_servo(float pitch);
    void update_pitch_cr_servo(float pitch);
    void update_pitch_onoff_servo(float pitch);
    void update_yaw_servo(float yaw);
    void update_yaw_position_servo(float yaw);
    void update_yaw_cr_servo(float yaw);
    void update_yaw_onoff_servo(float yaw);
    void init_tracker();
    void update_notify();
    bool get_home_eeprom(struct Location &loc);
    void set_home_eeprom(struct Location temp);
    void set_home(struct Location temp);
    void arm_servos();
    void disarm_servos();
    void prepare_servos();
    void set_mode(enum ControlMode mode);
    bool mavlink_set_mode(uint8_t mode);
    void check_usb_mux(void);
    void update_vehicle_pos_estimate();
    void update_tracker_position();
    void update_bearing_and_distance();
    void update_tracking(void);
    void tracking_update_position(const mavlink_global_position_int_t &msg);
    void tracking_update_pressure(const mavlink_scaled_pressure_t &msg);
    void tracking_manual_control(const mavlink_manual_control_t &msg);
    void update_armed_disarmed();
    void gcs_send_text_fmt(const prog_char_t *fmt, ...);
    void init_capabilities(void);

public:
    void mavlink_snoop(const mavlink_message_t* msg);
    void mavlink_delay_cb();
};

#define MENU_FUNC(func) FUNCTOR_BIND(&tracker, &Tracker::func, int8_t, uint8_t, const Menu::arg *)

extern const AP_HAL::HAL& hal;
extern Tracker tracker;
