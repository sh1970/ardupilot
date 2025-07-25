#include "Copter.h"

#if MODE_AUTO_ENABLED

/*
 * Init and run calls for auto flight mode
 *
 * This file contains the implementation for Land, Waypoint navigation and Takeoff from Auto mode
 * Command execution code (i.e. command_logic.pde) should:
 *      a) switch to Auto flight mode with set_mode() function.  This will cause auto_init to be called
 *      b) call one of the three auto initialisation functions: auto_wp_start(), auto_takeoff_start(), auto_land_start()
 *      c) call one of the verify functions auto_wp_verify(), auto_takeoff_verify, auto_land_verify repeated to check if the command has completed
 * The main loop (i.e. fast loop) will call update_flight_modes() which will in turn call auto_run() which, based upon the auto_mode variable will call
 *      correct auto_wp_run, auto_takeoff_run or auto_land_run to actually implement the feature
 */

/*
 *  While in the auto flight mode, navigation or do/now commands can be run.
 *  Code in this file implements the navigation commands
 */

// auto_init - initialise auto controller
bool ModeAuto::init(bool ignore_checks)
{
    auto_RTL = false;
    if (mission.present() || ignore_checks) {
        // reject switching to auto mode if landed with motors armed but first command is not a takeoff (reduce chance of flips)
        if (motors->armed() && copter.ap.land_complete && !mission.starts_with_takeoff_cmd()) {
            gcs().send_text(MAV_SEVERITY_CRITICAL, "Auto: Missing Takeoff Cmd");
            return false;
        }

        _mode = SubMode::LOITER;

        // stop ROI from carrying over from previous runs of the mission
        // To-Do: reset the yaw as part of auto_wp_start when the previous command was not a wp command to remove the need for this special ROI check
        if (auto_yaw.mode() == AutoYaw::Mode::ROI) {
            auto_yaw.set_mode(AutoYaw::Mode::HOLD);
        }

        // initialise waypoint and spline controller
        wp_nav->wp_and_spline_init_cm();

        // initialise desired speed overrides
        desired_speed_override = {0, 0, 0};

        // set flag to start mission
        waiting_to_start = true;

        // initialise mission change check (ignore results)
        IGNORE_RETURN(mis_change_detector.check_for_mission_change());

        // clear guided limits
        copter.mode_guided.limit_clear();

        // reset flag indicating if pilot has applied roll or pitch inputs during landing
        copter.ap.land_repo_active = false;

#if AC_PRECLAND_ENABLED
        // initialise precland state machine
        copter.precland_statemachine.init();
#endif

        return true;
    } else {
        return false;
    }
}

// stop mission when we leave auto mode
void ModeAuto::exit()
{
    if (copter.mode_auto.mission.state() == AP_Mission::MISSION_RUNNING) {
        copter.mode_auto.mission.stop();
    }
#if HAL_MOUNT_ENABLED
    copter.camera_mount.set_mode_to_default();
#endif  // HAL_MOUNT_ENABLED

    auto_RTL = false;
}

// auto_run - runs the auto controller
//      should be called at 100hz or more
void ModeAuto::run()
{
    // start or update mission
    if (waiting_to_start) {
        // don't start the mission until we have an origin
        Location loc;
        if (copter.ahrs.get_origin(loc)) {
            // start/resume the mission (based on MIS_RESTART parameter)
            mission.start_or_resume();
            waiting_to_start = false;

            // initialise mission change check (ignore results)
            IGNORE_RETURN(mis_change_detector.check_for_mission_change());
        }
    } else {
        // check for mission changes
        if (mis_change_detector.check_for_mission_change()) {
            // if mission is running restart the current command if it is a waypoint or spline command
            if ((mission.state() == AP_Mission::MISSION_RUNNING) && (_mode == SubMode::WP)) {
                if (mission.restart_current_nav_cmd()) {
                    gcs().send_text(MAV_SEVERITY_CRITICAL, "Auto mission changed, restarted command");
                } else {
                    // failed to restart mission for some reason
                    gcs().send_text(MAV_SEVERITY_CRITICAL, "Auto mission changed but failed to restart command");
                }
            }
        }

        mission.update();
    }

    // call the correct auto controller
    switch (_mode) {

    case SubMode::TAKEOFF:
        takeoff_run();
        break;

    case SubMode::WP:
    case SubMode::CIRCLE_MOVE_TO_EDGE:
        wp_run();
        break;

    case SubMode::LAND:
        land_run();
        break;

    case SubMode::RTL:
        rtl_run();
        break;

    case SubMode::CIRCLE:
        circle_run();
        break;

    case SubMode::NAVGUIDED:
    case SubMode::NAV_SCRIPT_TIME:
#if AC_NAV_GUIDED || AP_SCRIPTING_ENABLED
        nav_guided_run();
#endif
        break;

    case SubMode::LOITER:
        loiter_run();
        break;

    case SubMode::LOITER_TO_ALT:
        loiter_to_alt_run();
        break;

#if AP_MISSION_NAV_PAYLOAD_PLACE_ENABLED && AC_PAYLOAD_PLACE_ENABLED
    case SubMode::NAV_PAYLOAD_PLACE:
        payload_place.run();
        break;
#endif

    case SubMode::NAV_ATTITUDE_TIME:
        nav_attitude_time_run();
        break;
    }

    // only pretend to be in auto RTL so long as mission still thinks its in a landing sequence or the mission has completed
    const bool auto_rtl_active = mission.get_in_landing_sequence_flag() || mission.get_in_return_path_flag() || mission.state() == AP_Mission::mission_state::MISSION_COMPLETE;
    if (auto_RTL && !auto_rtl_active) {
        auto_RTL = false;
        // log exit from Auto RTL
#if HAL_LOGGING_ENABLED
        copter.logger.Write_Mode((uint8_t)copter.flightmode->mode_number(), ModeReason::AUTO_RTL_EXIT);
#endif
    }
}

// return true if a position estimate is required
bool ModeAuto::requires_GPS() const
{
    // position estimate is required in all sub modes except attitude control
    return _mode != SubMode::NAV_ATTITUDE_TIME;
}

// set submode.  This may re-trigger the vehicle's EKF failsafe if the new submode requires a position estimate
void ModeAuto::set_submode(SubMode new_submode)
{
    // return immediately if the submode has not been changed
    if (new_submode == _mode) {
        return;
    }

    // backup old mode
    SubMode old_submode = _mode;

    // set mode
    _mode = new_submode;

    // if changing out of the nav-attitude-time submode, recheck the EKF failsafe
    // this may trigger a flight mode change if the EKF failsafe is active
    if (old_submode == SubMode::NAV_ATTITUDE_TIME) {
        copter.failsafe_ekf_recheck();
    }
}

bool ModeAuto::option_is_enabled(Option option) const
{
    return ((copter.g2.auto_options & (uint32_t)option) != 0);
}

bool ModeAuto::allows_arming(AP_Arming::Method method) const
{
    if (auto_RTL) {
        return false;
    }
    return option_is_enabled(Option::AllowArming);
}

#if WEATHERVANE_ENABLED
bool ModeAuto::allows_weathervaning() const
{
    return option_is_enabled(Option::AllowWeatherVaning);
}
#endif

// Go straight to landing sequence via DO_LAND_START, if succeeds pretend to be Auto RTL mode
bool ModeAuto::jump_to_landing_sequence_auto_RTL(ModeReason reason)
{
    if (!mission.jump_to_landing_sequence(get_stopping_point())) {
        LOGGER_WRITE_ERROR(LogErrorSubsystem::FLIGHT_MODE, LogErrorCode(Number::AUTO_RTL));
        // make sad noise
        if (copter.ap.initialised) {
            AP_Notify::events.user_mode_change_failed = 1;
        }
        gcs().send_text(MAV_SEVERITY_WARNING, "Mode change to AUTO RTL failed: No landing sequence found");
        return false;
    }

    return enter_auto_rtl(reason);
}

// Join mission after DO_RETURN_PATH_START waypoint, if succeeds pretend to be Auto RTL mode
bool ModeAuto::return_path_start_auto_RTL(ModeReason reason)
{
    if (!mission.jump_to_closest_mission_leg(get_stopping_point())) {
        LOGGER_WRITE_ERROR(LogErrorSubsystem::FLIGHT_MODE, LogErrorCode(Number::AUTO_RTL));
        // make sad noise
        if (copter.ap.initialised) {
            AP_Notify::events.user_mode_change_failed = 1;
        }
        gcs().send_text(MAV_SEVERITY_WARNING, "Mode change to AUTO RTL failed: No return path found");
        return false;
    }

    return enter_auto_rtl(reason);
}

// Try join return path else do land start
bool ModeAuto::return_path_or_jump_to_landing_sequence_auto_RTL(ModeReason reason)
{
    const Location stopping_point = get_stopping_point();
    if (!mission.jump_to_closest_mission_leg(stopping_point) && !mission.jump_to_landing_sequence(stopping_point)) {
        LOGGER_WRITE_ERROR(LogErrorSubsystem::FLIGHT_MODE, LogErrorCode(Number::AUTO_RTL));
        // make sad noise
        if (copter.ap.initialised) {
            AP_Notify::events.user_mode_change_failed = 1;
        }
        gcs().send_text(MAV_SEVERITY_WARNING, "Mode change to AUTO RTL failed: No return path or landing sequence found");
        return false;
    }

    return enter_auto_rtl(reason);
}

// Enter auto rtl pseudo mode
bool ModeAuto::enter_auto_rtl(ModeReason reason) 
{
    mission.set_force_resume(true);

    // if not already in auto switch to auto
    if ((copter.flightmode == this) || set_mode(Mode::Number::AUTO, reason)) {
        auto_RTL = true;
#if HAL_LOGGING_ENABLED
        // log entry into AUTO RTL
        copter.logger.Write_Mode((uint8_t)copter.flightmode->mode_number(), reason);
#endif

        // make happy noise
        if (copter.ap.initialised) {
            AP_Notify::events.user_mode_change = 1;
        }
        return true;
    }

    // mode change failed, revert force resume flag
    mission.set_force_resume(false);

    LOGGER_WRITE_ERROR(LogErrorSubsystem::FLIGHT_MODE, LogErrorCode(Number::AUTO_RTL));
    // make sad noise
    if (copter.ap.initialised) {
        AP_Notify::events.user_mode_change_failed = 1;
    }
    return false;
}

// lua scripts use this to retrieve the contents of the active command
bool ModeAuto::nav_script_time(uint16_t &id, uint8_t &cmd, float &arg1, float &arg2, int16_t &arg3, int16_t &arg4)
{
#if AP_SCRIPTING_ENABLED
    if (_mode == SubMode::NAV_SCRIPT_TIME) {
        id = nav_scripting.id;
        cmd = nav_scripting.command;
        arg1 = nav_scripting.arg1;
        arg2 = nav_scripting.arg2;
        arg3 = nav_scripting.arg3;
        arg4 = nav_scripting.arg4;
        return true;
    }
#endif
    return false;
}

// lua scripts use this to indicate when they have complete the command
void ModeAuto::nav_script_time_done(uint16_t id)
{
#if AP_SCRIPTING_ENABLED
    if ((_mode == SubMode::NAV_SCRIPT_TIME) && (id == nav_scripting.id)) {
        nav_scripting.done = true;
    }
#endif
}

// auto_loiter_start - initialises loitering in auto mode
//  returns success/failure because this can be called by exit_mission
bool ModeAuto::loiter_start()
{
    // return failure if GPS is bad
    if (!copter.position_ok()) {
        return false;
    }
    _mode = SubMode::LOITER;

    // calculate stopping point
    Vector3f stopping_point;
    wp_nav->get_wp_stopping_point_NEU_cm(stopping_point);

    // initialise waypoint controller target to stopping point
    wp_nav->set_wp_destination_NEU_cm(stopping_point);

    // hold yaw at current heading
    auto_yaw.set_mode(AutoYaw::Mode::HOLD);

    return true;
}

// auto_rtl_start - initialises RTL in AUTO flight mode
void ModeAuto::rtl_start()
{
    // call regular rtl flight mode initialisation and ask it to ignore checks
    if (copter.mode_rtl.init(true)) {
        set_submode(SubMode::RTL);
    } else {
        // this should never happen because RTL never fails init if argument is true
        INTERNAL_ERROR(AP_InternalError::error_t::flow_of_control);
    }
}

// initialise waypoint controller to implement take-off
void ModeAuto::takeoff_start(const Location& dest_loc)
{
    if (!copter.current_loc.initialised()) {
        // this should never happen because mission commands are not executed until
        // the AHRS/EKF origin is set by which time current_loc should also have been set
        INTERNAL_ERROR(AP_InternalError::error_t::flow_of_control);
        return;
    }

    // calculate current and target altitudes
    // by default current_alt_cm and alt_target_cm are alt-above-EKF-origin
    int32_t alt_target_cm;
    bool alt_target_terrain = false;
    float current_alt_cm = pos_control->get_pos_estimate_NEU_cm().z;
    float terrain_offset;   // terrain's altitude in cm above the ekf origin
    if ((dest_loc.get_alt_frame() == Location::AltFrame::ABOVE_TERRAIN) && wp_nav->get_terrain_offset_cm(terrain_offset)) {
        // subtract terrain offset to convert vehicle's alt-above-ekf-origin to alt-above-terrain
        current_alt_cm -= terrain_offset;

        // specify alt_target_cm as alt-above-terrain
        alt_target_cm = dest_loc.alt;
        alt_target_terrain = true;
    } else {
        // set horizontal target
        Location dest(dest_loc);
        dest.lat = copter.current_loc.lat;
        dest.lng = copter.current_loc.lng;

        // get altitude target above EKF origin
        if (!dest.get_alt_cm(Location::AltFrame::ABOVE_ORIGIN, alt_target_cm)) {
            // this failure could only happen if take-off alt was specified as an alt-above terrain and we have no terrain data
            LOGGER_WRITE_ERROR(LogErrorSubsystem::TERRAIN, LogErrorCode::MISSING_TERRAIN_DATA);
            // fall back to altitude above current altitude
            alt_target_cm = current_alt_cm + dest.alt;
        }
    }

    // sanity check target
    int32_t alt_target_min_cm = current_alt_cm + (copter.ap.land_complete ? 100 : 0);
    alt_target_cm = MAX(alt_target_cm, alt_target_min_cm);

    // initialise yaw
    auto_yaw.set_mode(AutoYaw::Mode::HOLD);

    // clear i term when we're taking off
    pos_control->init_U_controller();

    // initialise alt for WP_NAVALT_MIN and set completion alt
    auto_takeoff.start(alt_target_cm, alt_target_terrain);

    // set submode
    set_submode(SubMode::TAKEOFF);
}

// auto_wp_start - initialises waypoint controller to implement flying to a particular destination
bool ModeAuto::wp_start(const Location& dest_loc)
{
    // init wpnav and set origin if transitioning from takeoff
    if (!wp_nav->is_active()) {
        Vector3f stopping_point;
        if (_mode == SubMode::TAKEOFF) {
            Vector3p takeoff_complete_pos;
            if (auto_takeoff.get_completion_pos(takeoff_complete_pos)) {
                stopping_point = takeoff_complete_pos.tofloat();
            }
        }
        float des_speed_xy_cm = is_positive(desired_speed_override.xy) ? (desired_speed_override.xy * 100) : 0;
        wp_nav->wp_and_spline_init_cm(des_speed_xy_cm, stopping_point);

        // override speeds up and down if necessary
        if (is_positive(desired_speed_override.up)) {
            wp_nav->set_speed_up_cms(desired_speed_override.up * 100.0);
        }
        if (is_positive(desired_speed_override.down)) {
            wp_nav->set_speed_down_cms(desired_speed_override.down * 100.0);
        }
    }

    if (!wp_nav->set_wp_destination_loc(dest_loc)) {
        return false;
    }

    // initialise yaw
    // To-Do: reset the yaw only when the previous navigation command is not a WP.  this would allow removing the special check for ROI
    if (auto_yaw.mode() != AutoYaw::Mode::ROI && !(auto_yaw.mode() == AutoYaw::Mode::FIXED && copter.g.wp_yaw_behavior == WP_YAW_BEHAVIOR_NONE)) {
        auto_yaw.set_mode_to_default(false);
    }

    // set submode
    set_submode(SubMode::WP);

    return true;
}

// auto_land_start - initialises controller to implement a landing
void ModeAuto::land_start()
{
    // set horizontal speed and acceleration limits
    pos_control->set_max_speed_accel_NE_cm(wp_nav->get_default_speed_NE_cms(), wp_nav->get_wp_acceleration_cmss());
    pos_control->set_correction_speed_accel_NE_cm(wp_nav->get_default_speed_NE_cms(), wp_nav->get_wp_acceleration_cmss());

    // initialise the vertical position controller
    if (!pos_control->is_active_NE()) {
        pos_control->init_NE_controller();
    }

    // set vertical speed and acceleration limits
    pos_control->set_max_speed_accel_U_cm(wp_nav->get_default_speed_down_cms(), wp_nav->get_default_speed_up_cms(), wp_nav->get_accel_U_cmss());
    pos_control->set_correction_speed_accel_U_cmss(wp_nav->get_default_speed_down_cms(), wp_nav->get_default_speed_up_cms(), wp_nav->get_accel_U_cmss());

    // initialise the vertical position controller
    if (!pos_control->is_active_U()) {
        pos_control->init_U_controller();
    }

    // initialise yaw
    auto_yaw.set_mode(AutoYaw::Mode::HOLD);

#if AP_LANDINGGEAR_ENABLED
    // optionally deploy landing gear
    copter.landinggear.deploy_for_landing();
#endif

    // reset flag indicating if pilot has applied roll or pitch inputs during landing
    copter.ap.land_repo_active = false;

    // this will be set true if prec land is later active
    copter.ap.prec_land_active = false;

    // set submode
    set_submode(SubMode::LAND);
}

// circle_movetoedge_start - initialise waypoint controller to move to edge of a circle with it's center at the specified location
//  we assume the caller has performed all required GPS_ok checks
void ModeAuto::circle_movetoedge_start(const Location &circle_center, float radius_m, bool ccw_turn)
{
    // set circle center
    copter.circle_nav->set_center(circle_center);

    // set circle radius
    if (!is_zero(radius_m)) {
        copter.circle_nav->set_radius_cm(radius_m * 100.0f);
    }

    // set circle direction by using rate
    float current_rate = copter.circle_nav->get_rate_degs();
    current_rate = ccw_turn ? -fabsf(current_rate) : fabsf(current_rate);
    copter.circle_nav->set_rate_degs(current_rate);

    // check our distance from edge of circle
    Vector3f circle_edge_neu;
    float dist_to_edge;
    copter.circle_nav->get_closest_point_on_circle_NEU_cm(circle_edge_neu, dist_to_edge);

    // if more than 3m then fly to edge
    if (dist_to_edge > 300.0f) {
        // convert circle_edge_neu to Location
        Location circle_edge(circle_edge_neu, Location::AltFrame::ABOVE_ORIGIN);

        // convert altitude to same as command
        circle_edge.copy_alt_from(circle_center);

        // initialise wpnav to move to edge of circle
        if (!wp_nav->set_wp_destination_loc(circle_edge)) {
            // failure to set destination can only be because of missing terrain data
            copter.failsafe_terrain_on_event();
        }

        // if we are outside the circle, point at the edge, otherwise hold yaw
        const float dist_to_center = get_horizontal_distance(pos_control->get_pos_estimate_NEU_cm().xy().tofloat(), copter.circle_nav->get_center_NEU_cm().xy().tofloat());
        // initialise yaw
        // To-Do: reset the yaw only when the previous navigation command is not a WP.  this would allow removing the special check for ROI
        if (auto_yaw.mode() != AutoYaw::Mode::ROI) {
            if (dist_to_center > copter.circle_nav->get_radius_cm() && dist_to_center > 500) {
                auto_yaw.set_mode_to_default(false);
            } else {
                // vehicle is within circle so hold yaw to avoid spinning as we move to edge of circle
                auto_yaw.set_mode(AutoYaw::Mode::HOLD);
            }
        }

        // set the submode to move to the edge of the circle
        set_submode(SubMode::CIRCLE_MOVE_TO_EDGE);
    } else {
        circle_start();
    }
}

// auto_circle_start - initialises controller to fly a circle in AUTO flight mode
//   assumes that circle_nav object has already been initialised with circle center and radius
void ModeAuto::circle_start()
{
    // initialise circle controller
    copter.circle_nav->init_NEU_cm(copter.circle_nav->get_center_NEU_cm(), copter.circle_nav->center_is_terrain_alt(), copter.circle_nav->get_rate_degs());

    if (auto_yaw.mode() != AutoYaw::Mode::ROI) {
        auto_yaw.set_mode(AutoYaw::Mode::CIRCLE);
    }

    // set submode to circle
    set_submode(SubMode::CIRCLE);
}

#if AC_NAV_GUIDED
// auto_nav_guided_start - hand over control to external navigation controller in AUTO mode
void ModeAuto::nav_guided_start()
{
    // call regular guided flight mode initialisation
    if (!copter.mode_guided.init(true)) {
        // this should never happen because guided mode never fails to init
        INTERNAL_ERROR(AP_InternalError::error_t::flow_of_control);
        return;
    }

    // initialise guided start time and position as reference for limit checking
    copter.mode_guided.limit_init_time_and_pos();

    // set submode
    set_submode(SubMode::NAVGUIDED);
}
#endif //AC_NAV_GUIDED

bool ModeAuto::is_landing() const
{
    switch(_mode) {
    case SubMode::LAND:
        return true;
    case SubMode::RTL:
        return copter.mode_rtl.is_landing();
    default:
        return false;
    }
    return false;
}

bool ModeAuto::is_taking_off() const
{
    return ((_mode == SubMode::TAKEOFF) && !auto_takeoff.complete);
}

#if AC_PAYLOAD_PLACE_ENABLED
// auto_payload_place_start - initialises controller to implement a placing
void PayloadPlace::start_descent()
{
    auto *pos_control = copter.pos_control;
    auto *wp_nav = copter.wp_nav;

    // set horizontal speed and acceleration limits
    pos_control->set_max_speed_accel_NE_cm(wp_nav->get_default_speed_NE_cms(), wp_nav->get_wp_acceleration_cmss());
    pos_control->set_correction_speed_accel_NE_cm(wp_nav->get_default_speed_NE_cms(), wp_nav->get_wp_acceleration_cmss());

    // initialise the vertical position controller
    if (!pos_control->is_active_NE()) {
        pos_control->init_NE_controller();
    }

    // set vertical speed and acceleration limits
    pos_control->set_max_speed_accel_U_cm(wp_nav->get_default_speed_down_cms(), wp_nav->get_default_speed_up_cms(), wp_nav->get_accel_U_cmss());
    pos_control->set_correction_speed_accel_U_cmss(wp_nav->get_default_speed_down_cms(), wp_nav->get_default_speed_up_cms(), wp_nav->get_accel_U_cmss());

    // initialise the vertical position controller
    if (!pos_control->is_active_U()) {
        pos_control->init_U_controller();
    }

    // initialise yaw
    copter.flightmode->auto_yaw.set_mode(Mode::AutoYaw::Mode::HOLD);

    state = PayloadPlace::State::Descent_Start;
}
#endif

// returns true if pilot's yaw input should be used to adjust vehicle's heading
bool ModeAuto::use_pilot_yaw(void) const
{
    const bool allow_yaw_option = !option_is_enabled(Option::IgnorePilotYaw);
    const bool rtl_allow_yaw = (_mode == SubMode::RTL) && copter.mode_rtl.use_pilot_yaw();
    const bool landing = _mode == SubMode::LAND;
    return allow_yaw_option || rtl_allow_yaw || landing;
}

bool ModeAuto::set_speed_xy_cms(float speed_xy_cms)
{
    copter.wp_nav->set_speed_NE_cms(speed_xy_cms);
    desired_speed_override.xy = speed_xy_cms * 0.01;
    return true;
}

bool ModeAuto::set_speed_up_cms(float speed_up_cms)
{
    copter.wp_nav->set_speed_up_cms(speed_up_cms);
    desired_speed_override.up = speed_up_cms * 0.01;
    return true;
}

bool ModeAuto::set_speed_down_cms(float speed_down_cms)
{
    copter.wp_nav->set_speed_down_cms(speed_down_cms);
    desired_speed_override.down = speed_down_cms * 0.01;
    return true;
}

// start_command - this function will be called when the ap_mission lib wishes to start a new command
bool ModeAuto::start_command(const AP_Mission::Mission_Command& cmd)
{
    switch(cmd.id) {

    ///
    /// navigation commands
    ///
    case MAV_CMD_NAV_VTOL_TAKEOFF:
    case MAV_CMD_NAV_TAKEOFF:                   // 22
        do_takeoff(cmd);
        break;

    case MAV_CMD_NAV_WAYPOINT:                  // 16  Navigate to Waypoint
        do_nav_wp(cmd);
        break;

    case MAV_CMD_NAV_VTOL_LAND:
    case MAV_CMD_NAV_LAND:              // 21 LAND to Waypoint
        do_land(cmd);
        break;

    case MAV_CMD_NAV_LOITER_UNLIM:              // 17 Loiter indefinitely
        do_loiter_unlimited(cmd);
        break;

    case MAV_CMD_NAV_LOITER_TURNS:              //18 Loiter N Times
        do_circle(cmd);
        break;

    case MAV_CMD_NAV_LOITER_TIME:              // 19
        do_loiter_time(cmd);
        break;

    case MAV_CMD_NAV_LOITER_TO_ALT:
        do_loiter_to_alt(cmd);
        break;

    case MAV_CMD_NAV_RETURN_TO_LAUNCH:             //20
        do_RTL();
        break;

    case MAV_CMD_NAV_SPLINE_WAYPOINT:           // 82  Navigate to Waypoint using spline
        do_spline_wp(cmd);
        break;

#if AC_NAV_GUIDED
    case MAV_CMD_NAV_GUIDED_ENABLE:             // 92  accept navigation commands from external nav computer
        do_nav_guided_enable(cmd);
        break;
#endif

    case MAV_CMD_NAV_DELAY:                    // 93 Delay the next navigation command
        do_nav_delay(cmd);
        break;

#if AP_MISSION_NAV_PAYLOAD_PLACE_ENABLED && AC_PAYLOAD_PLACE_ENABLED
    case MAV_CMD_NAV_PAYLOAD_PLACE:              // 94 place at Waypoint
        do_payload_place(cmd);
        break;
#endif

#if AP_SCRIPTING_ENABLED
    case MAV_CMD_NAV_SCRIPT_TIME:
        do_nav_script_time(cmd);
        break;
#endif

    case MAV_CMD_NAV_ATTITUDE_TIME:
        do_nav_attitude_time(cmd);
        break;

    //
    // conditional commands
    //
    case MAV_CMD_CONDITION_DELAY:             // 112
        do_wait_delay(cmd);
        break;

    case MAV_CMD_CONDITION_DISTANCE:             // 114
        do_within_distance(cmd);
        break;

    case MAV_CMD_CONDITION_YAW:             // 115
        do_yaw(cmd);
        break;

    ///
    /// do commands
    ///
    case MAV_CMD_DO_CHANGE_SPEED:             // 178
        do_change_speed(cmd);
        break;

    case MAV_CMD_DO_SET_HOME:             // 179
        do_set_home(cmd);
        break;

    case MAV_CMD_DO_SET_ROI_LOCATION:       // 195
    case MAV_CMD_DO_SET_ROI_NONE:           // 197
    case MAV_CMD_DO_SET_ROI:                // 201
        // point the copter and camera at a region of interest (ROI)
        // ROI_NONE can be handled by the regular ROI handler because lat, lon, alt are always zero
        do_roi(cmd);
        break;

#if HAL_MOUNT_ENABLED
    case MAV_CMD_DO_MOUNT_CONTROL:          // 205
        // point the camera to a specified angle
        do_mount_control(cmd);
        break;
#endif

#if AC_NAV_GUIDED
    case MAV_CMD_DO_GUIDED_LIMITS:                      // 220  accept guided mode limits
        do_guided_limits(cmd);
        break;
#endif

#if AP_WINCH_ENABLED
    case MAV_CMD_DO_WINCH:                             // Mission command to control winch
        do_winch(cmd);
        break;
#endif

    case MAV_CMD_DO_RETURN_PATH_START:
    case MAV_CMD_DO_LAND_START:
        break;

    default:
        // unable to use the command, allow the vehicle to try the next command
        return false;
    }

    // always return success
    return true;
}

// exit_mission - function that is called once the mission completes
void ModeAuto::exit_mission()
{
    // play a tone
    AP_Notify::events.mission_complete = 1;
    // if we are not on the ground switch to loiter or land
    if (!copter.ap.land_complete) {
        // try to enter loiter but if that fails land
        if (!loiter_start()) {
            set_mode(Mode::Number::LAND, ModeReason::MISSION_END);
        }
    } else {
        // if we've landed it's safe to disarm
        copter.arming.disarm(AP_Arming::Method::MISSIONEXIT);
    }
}

// do_guided - start guided mode
bool ModeAuto::do_guided(const AP_Mission::Mission_Command& cmd)
{
    // only process guided waypoint if we are in guided mode
    if (!copter.flightmode->in_guided_mode()) {
        return false;
    }

    // switch to handle different commands
    switch (cmd.id) {

        case MAV_CMD_NAV_WAYPOINT:
        {
            // set wp_nav's destination
            Location dest(cmd.content.location);
            return copter.mode_guided.set_destination(dest);
        }

        case MAV_CMD_CONDITION_YAW:
            do_yaw(cmd);
            return true;

        default:
            // reject unrecognised command
            return false;
    }

    return true;
}

float ModeAuto::wp_distance_m() const
{
    switch (_mode) {
    case SubMode::CIRCLE:
        return copter.circle_nav->get_distance_to_target_cm() * 0.01f;
    case SubMode::WP:
    case SubMode::CIRCLE_MOVE_TO_EDGE:
    default:
        return wp_nav->get_wp_distance_to_destination_cm() * 0.01f;
    }
}

float ModeAuto::wp_bearing_deg() const
{
    switch (_mode) {
    case SubMode::CIRCLE:
        return degrees(copter.circle_nav->get_bearing_to_target_rad());
    case SubMode::WP:
    case SubMode::CIRCLE_MOVE_TO_EDGE:
    default:
        return degrees(wp_nav->get_wp_bearing_to_destination_rad());
    }
}

bool ModeAuto::get_wp(Location& destination) const
{
    switch (_mode) {
    case SubMode::NAVGUIDED:
        return copter.mode_guided.get_wp(destination);
    case SubMode::WP:
        return wp_nav->get_oa_wp_destination(destination);
    case SubMode::RTL:
        return copter.mode_rtl.get_wp(destination);
    default:
        return false;
    }
}

/*******************************************************************************
Verify command Handlers

Each type of mission element has a "verify" operation. The verify
operation returns true when the mission element has completed and we
should move onto the next mission element.
Return true if we do not recognize the command so that we move on to the next command
*******************************************************************************/

// verify_command - callback function called from ap-mission at 10hz or higher when a command is being run
//      we double check that the flight mode is AUTO to avoid the possibility of ap-mission triggering actions while we're not in AUTO mode
bool ModeAuto::verify_command(const AP_Mission::Mission_Command& cmd)
{
    if (copter.flightmode != &copter.mode_auto) {
        return false;
    }

    bool cmd_complete = false;

    switch (cmd.id) {
    //
    // navigation commands
    //
    case MAV_CMD_NAV_VTOL_TAKEOFF:
    case MAV_CMD_NAV_TAKEOFF:
        cmd_complete = verify_takeoff();
        break;

    case MAV_CMD_NAV_WAYPOINT:
        cmd_complete = verify_nav_wp(cmd);
        break;

    case MAV_CMD_NAV_VTOL_LAND:
    case MAV_CMD_NAV_LAND:
        cmd_complete = verify_land();
        break;

#if AP_MISSION_NAV_PAYLOAD_PLACE_ENABLED && AC_PAYLOAD_PLACE_ENABLED
    case MAV_CMD_NAV_PAYLOAD_PLACE:
        cmd_complete = payload_place.verify();
        break;
#endif

    case MAV_CMD_NAV_LOITER_UNLIM:
        cmd_complete = verify_loiter_unlimited();
        break;

    case MAV_CMD_NAV_LOITER_TURNS:
        cmd_complete = verify_circle(cmd);
        break;

    case MAV_CMD_NAV_LOITER_TIME:
        cmd_complete = verify_loiter_time(cmd);
        break;

    case MAV_CMD_NAV_LOITER_TO_ALT:
        return verify_loiter_to_alt();

    case MAV_CMD_NAV_RETURN_TO_LAUNCH:
        cmd_complete = verify_RTL();
        break;

    case MAV_CMD_NAV_SPLINE_WAYPOINT:
        cmd_complete = verify_spline_wp(cmd);
        break;

#if AC_NAV_GUIDED
    case MAV_CMD_NAV_GUIDED_ENABLE:
        cmd_complete = verify_nav_guided_enable(cmd);
        break;
#endif

     case MAV_CMD_NAV_DELAY:
        cmd_complete = verify_nav_delay(cmd);
        break;

#if AP_SCRIPTING_ENABLED
    case MAV_CMD_NAV_SCRIPT_TIME:
        cmd_complete = verify_nav_script_time();
        break;
#endif

    case MAV_CMD_NAV_ATTITUDE_TIME:
        cmd_complete = verify_nav_attitude_time(cmd);
        break;

    ///
    /// conditional commands
    ///
    case MAV_CMD_CONDITION_DELAY:
        cmd_complete = verify_wait_delay();
        break;

    case MAV_CMD_CONDITION_DISTANCE:
        cmd_complete = verify_within_distance();
        break;

    case MAV_CMD_CONDITION_YAW:
        cmd_complete = verify_yaw();
        break;

    // do commands (always return true)
    case MAV_CMD_DO_CHANGE_SPEED:
    case MAV_CMD_DO_SET_HOME:
    case MAV_CMD_DO_SET_ROI_LOCATION:
    case MAV_CMD_DO_SET_ROI_NONE:
    case MAV_CMD_DO_SET_ROI:
#if HAL_MOUNT_ENABLED
    case MAV_CMD_DO_MOUNT_CONTROL:
#endif
#if AC_NAV_GUIDED
    case MAV_CMD_DO_GUIDED_LIMITS:
#endif
#if AP_FENCE_ENABLED
    case MAV_CMD_DO_FENCE_ENABLE:
#endif
#if AP_WINCH_ENABLED
    case MAV_CMD_DO_WINCH:
#endif
    case MAV_CMD_DO_RETURN_PATH_START:
    case MAV_CMD_DO_LAND_START:
        cmd_complete = true;
        break;

    default:
        // error message
        gcs().send_text(MAV_SEVERITY_WARNING,"Skipping invalid cmd #%i",cmd.id);
        // return true if we do not recognize the command so that we move on to the next command
        cmd_complete = true;
        break;
    }


    // send message to GCS
    if (cmd_complete) {
        gcs().send_mission_item_reached_message(cmd.index);
    }

    return cmd_complete;
}

// takeoff_run - takeoff in auto mode
//      called by auto_run at 100hz or more
void ModeAuto::takeoff_run()
{
    // if the user doesn't want to raise the throttle we can set it automatically
    // note that this can defeat the disarm check on takeoff
    if (option_is_enabled(Option::AllowTakeOffWithoutRaisingThrottle)) {
        copter.set_auto_armed(true);
    }
    auto_takeoff.run();
}

// auto_wp_run - runs the auto waypoint controller
//      called by auto_run at 100hz or more
void ModeAuto::wp_run()
{
    // if not armed set throttle to zero and exit immediately
    if (is_disarmed_or_landed()) {
        make_safe_ground_handling();
        return;
    }

    // set motors to full range
    motors->set_desired_spool_state(AP_Motors::DesiredSpoolState::THROTTLE_UNLIMITED);

    // run waypoint controller
    copter.failsafe_terrain_set_status(wp_nav->update_wpnav());

    // WP_Nav has set the vertical position control targets
    // run the vertical position controller and set output throttle
    pos_control->update_U_controller();

    // call attitude controller with auto yaw
    attitude_control->input_thrust_vector_heading(pos_control->get_thrust_vector(), auto_yaw.get_heading());
}

// auto_land_run - lands in auto mode
//      called by auto_run at 100hz or more
void ModeAuto::land_run()
{

    // if not armed set throttle to zero and exit immediately
    if (is_disarmed_or_landed()) {
        make_safe_ground_handling();
        return;
    }

    // set motors to full range
    motors->set_desired_spool_state(AP_Motors::DesiredSpoolState::THROTTLE_UNLIMITED);

    // run normal landing or precision landing (if enabled)
    land_run_normal_or_precland();
}

// auto_rtl_run - rtl in AUTO flight mode
//      called by auto_run at 100hz or more
void ModeAuto::rtl_run()
{
    // call regular rtl flight mode run function
    copter.mode_rtl.run(false);
}

// auto_circle_run - circle in AUTO flight mode
//      called by auto_run at 100hz or more
void ModeAuto::circle_run()
{
    // call circle controller
    copter.failsafe_terrain_set_status(copter.circle_nav->update_cms());

    // WP_Nav has set the vertical position control targets
    // run the vertical position controller and set output throttle
    pos_control->update_U_controller();

    // call attitude controller with auto yaw
    attitude_control->input_thrust_vector_heading(pos_control->get_thrust_vector(), auto_yaw.get_heading());
}

#if AC_NAV_GUIDED || AP_SCRIPTING_ENABLED
// auto_nav_guided_run - allows control by external navigation controller
//      called by auto_run at 100hz or more
void ModeAuto::nav_guided_run()
{
    // call regular guided flight mode run function
    copter.mode_guided.run();
}
#endif  // AC_NAV_GUIDED || AP_SCRIPTING_ENABLED

// auto_loiter_run - loiter in AUTO flight mode
//      called by auto_run at 100hz or more
void ModeAuto::loiter_run()
{
    // if not armed set throttle to zero and exit immediately
    if (is_disarmed_or_landed()) {
        make_safe_ground_handling();
        return;
    }

    // set motors to full range
    motors->set_desired_spool_state(AP_Motors::DesiredSpoolState::THROTTLE_UNLIMITED);

    // run waypoint and z-axis position controller
    copter.failsafe_terrain_set_status(wp_nav->update_wpnav());

    pos_control->update_U_controller();

    // call attitude controller with auto yaw
    attitude_control->input_thrust_vector_heading(pos_control->get_thrust_vector(), auto_yaw.get_heading());
}

// auto_loiter_run - loiter to altitude in AUTO flight mode
//      called by auto_run at 100hz or more
void ModeAuto::loiter_to_alt_run()
{
    // if not auto armed or motor interlock not enabled set throttle to zero and exit immediately
    if (is_disarmed_or_landed() || !motors->get_interlock()) {
        make_safe_ground_handling();
        return;
    }

    // possibly just run the waypoint controller:
    if (!loiter_to_alt.reached_destination_xy) {
        loiter_to_alt.reached_destination_xy = wp_nav->reached_wp_destination_NE();
        if (!loiter_to_alt.reached_destination_xy) {
            wp_run();
            return;
        }
    }

    if (!loiter_to_alt.loiter_start_done) {
        // set horizontal speed and acceleration limits
        pos_control->set_max_speed_accel_NE_cm(wp_nav->get_default_speed_NE_cms(), wp_nav->get_wp_acceleration_cmss());
        pos_control->set_correction_speed_accel_NE_cm(wp_nav->get_default_speed_NE_cms(), wp_nav->get_wp_acceleration_cmss());

        if (!pos_control->is_active_NE()) {
            pos_control->init_NE_controller();
        }

        loiter_to_alt.loiter_start_done = true;
    }
    const float alt_error_cm = copter.current_loc.alt - loiter_to_alt.alt;
    if (fabsf(alt_error_cm) < 5.0) { // random numbers R US
        loiter_to_alt.reached_alt = true;
    } else if (alt_error_cm * loiter_to_alt.alt_error_cm < 0) {
        // we were above and are now below, or vice-versa
        loiter_to_alt.reached_alt = true;
    }
    loiter_to_alt.alt_error_cm = alt_error_cm;

    // loiter...

    land_run_horizontal_control();

    // Compute a vertical velocity demand such that the vehicle
    // approaches the desired altitude.
    float target_climb_rate_cms = sqrt_controller(
        -alt_error_cm,
        pos_control->get_pos_U_p().kP(),
        pos_control->get_max_accel_U_cmss(),
        G_Dt);
    target_climb_rate_cms = constrain_float(target_climb_rate_cms, pos_control->get_max_speed_down_cms(), pos_control->get_max_speed_up_cms());

    // get avoidance adjusted climb rate
    target_climb_rate_cms = get_avoidance_adjusted_climbrate_cms(target_climb_rate_cms);

#if AP_RANGEFINDER_ENABLED
    // update the vertical offset based on the surface measurement
    copter.surface_tracking.update_surface_offset();
#endif

    // Send the commanded climb rate to the position controller
    pos_control->set_pos_target_U_from_climb_rate_cm(target_climb_rate_cms);

    pos_control->update_U_controller();
}

// maintain an attitude for a specified time
void ModeAuto::nav_attitude_time_run()
{
    // if not auto armed or motor interlock not enabled set throttle to zero and exit immediately
    if (is_disarmed_or_landed() || !motors->get_interlock()) {
        make_safe_ground_handling();
        return;
    }

    // constrain climb rate
    float target_climb_rate_cms = constrain_float(nav_attitude_time.climb_rate * 100.0, pos_control->get_max_speed_down_cms(), pos_control->get_max_speed_up_cms());

    // get avoidance adjusted climb rate
    target_climb_rate_cms = get_avoidance_adjusted_climbrate_cms(target_climb_rate_cms);

    // limit and scale lean angles
    // todo: change euler magnitiude limit to lean angle limit
    const float angle_limit_rad = MAX(radians(10.0f), MIN(attitude_control->lean_angle_max_rad(), attitude_control->get_althold_lean_angle_max_rad()));
    Vector2f target_rp_rad(radians(nav_attitude_time.roll_deg), radians(nav_attitude_time.pitch_deg));
    target_rp_rad.limit_length(angle_limit_rad);

    // send targets to attitude controller
    attitude_control->input_euler_angle_roll_pitch_yaw_rad(target_rp_rad.x, target_rp_rad.y, radians(nav_attitude_time.yaw_deg), true);

    // Send the commanded climb rate to the position controller
    pos_control->set_pos_target_U_from_climb_rate_cm(target_climb_rate_cms);

    pos_control->update_U_controller();
}

#if AC_PAYLOAD_PLACE_ENABLED
// auto_payload_place_run - places an object in auto mode
//      called by auto_run at 100hz or more
void PayloadPlace::run()
{
    const char* prefix_str = "PayloadPlace:";

    if (copter.flightmode->is_disarmed_or_landed()) {
        copter.flightmode->make_safe_ground_handling();
        return;
    }

    // set motors to full range
    copter.motors->set_desired_spool_state(AP_Motors::DesiredSpoolState::THROTTLE_UNLIMITED);

    const uint32_t descent_thrust_cal_duration_ms = 2000; // milliseconds
    const uint32_t placed_check_duration_ms = 500; // how long we have to be below a throttle threshold before considering placed

    auto &g2 = copter.g2;
    const auto &g = copter.g;
    auto *attitude_control = copter.attitude_control;
    const auto &pos_control = copter.pos_control;
    const auto &wp_nav = copter.wp_nav;

    // Vertical thrust is taken from the attitude controller before angle boost is added
    const float thrust_level = attitude_control->get_throttle_in();
    const uint32_t now_ms = AP_HAL::millis();

    // relax position target if we might be landed
    // if we discover we've landed then immediately release the load:
    if (copter.ap.land_complete || copter.ap.land_complete_maybe) {
        pos_control->soften_for_landing_NE();
        switch (state) {
        case State::FlyToLocation:
            // this is handled in wp_run()
            break;
        case State::Descent_Start:
            // do nothing on this loop
            break;
        case State::Descent:
            gcs().send_text(MAV_SEVERITY_INFO, "%s landed", prefix_str);
            state = State::Release;
            break;
        case State::Release:
        case State::Releasing:
        case State::Delay:
        case State::Ascent_Start:
        case State::Ascent:
        case State::Done:
            break;
        }
    }

#if AP_GRIPPER_ENABLED
    // if pilot releases load manually:
    if (AP::gripper().valid() && AP::gripper().released()) {
        switch (state) {
        case State::FlyToLocation:
        case State::Descent_Start:
            gcs().send_text(MAV_SEVERITY_INFO, "%s Abort: Gripper Open", prefix_str);
            // Descent_Start has not run so we must also initialise descent_start_altitude_cm
            descent_start_altitude_cm = pos_control->get_pos_desired_U_cm();
            state = State::Done;
            break;
        case State::Descent:
            gcs().send_text(MAV_SEVERITY_INFO, "%s Manual release", prefix_str);
            state = State::Release;
            break;
        case State::Release:
        case State::Releasing:
        case State::Delay:
        case State::Ascent_Start:
        case State::Ascent:
        case State::Done:
            break;
        }
    }
#endif

    switch (state) {
    case State::FlyToLocation:
        if (copter.wp_nav->reached_wp_destination()) {
            start_descent();
        }
        break;

    case State::Descent_Start:
        descent_established_time_ms = now_ms;
        descent_start_altitude_cm = pos_control->get_pos_desired_U_cm();
        // limiting the decent rate to the limit set in wp_nav is not necessary but done for safety
        descent_speed_cms = MIN((is_positive(g2.pldp_descent_speed_ms)) ? g2.pldp_descent_speed_ms * 100.0 : abs(g.land_speed), wp_nav->get_default_speed_down_cms());
        descent_thrust_level = 1.0;
        state = State::Descent;
        FALLTHROUGH;

    case State::Descent:
        // check maximum decent distance
        if (!is_zero(descent_max_cm) &&
            descent_start_altitude_cm - pos_control->get_pos_desired_U_cm() > descent_max_cm) {
            state = State::Ascent_Start;
            gcs().send_text(MAV_SEVERITY_WARNING, "%s Reached maximum descent", prefix_str);
            break;
        }
        // calibrate the decent thrust after aircraft has reached constant decent rate and release if threshold is reached
        if (pos_control->get_vel_desired_NEU_cms().z > -0.95 * descent_speed_cms) {
            // decent rate has not reached descent_speed_cms
            descent_established_time_ms = now_ms;
            break;
        } else if (now_ms - descent_established_time_ms < descent_thrust_cal_duration_ms) {
            // record minimum thrust for descent_thrust_cal_duration_ms
            descent_thrust_level = MIN(descent_thrust_level, thrust_level);
            place_start_time_ms = now_ms;
            break;
        } else if (thrust_level > g2.pldp_thrust_placed_fraction * descent_thrust_level) {
            // thrust is above minimum threshold
            place_start_time_ms = now_ms;
            break;
        } else if (is_positive(g2.pldp_range_finder_maximum_m)) {
            if (!copter.rangefinder_state.enabled) {
                // abort payload place because rangefinder is not enabled
                state = State::Ascent_Start;
                gcs().send_text(MAV_SEVERITY_WARNING, "%s PLDP_RNG_MAX set and rangefinder not enabled", prefix_str);
                break;
            } else if (copter.rangefinder_alt_ok() && (copter.rangefinder_state.glitch_count == 0) && (copter.rangefinder_state.alt_cm > g2.pldp_range_finder_maximum_m * 100.0)) {
                // range finder altitude is above maximum
                place_start_time_ms = now_ms;
                break;
            }
        }

        // If we get here:
        // 1. we have reached decent velocity
        // 2. measured the thrust level required for decent
        // 3. detected that our thrust requirements have reduced
        // 4. rangefinder range has dropped below minimum if set
        // 5. place_start_time_ms has been initialised

        // payload touchdown must be detected for 0.5 seconds

        if (now_ms - place_start_time_ms > placed_check_duration_ms) {
            state = State::Release;
            gcs().send_text(MAV_SEVERITY_INFO, "%s payload release thrust threshold: %f", prefix_str, static_cast<double>(g2.pldp_thrust_placed_fraction * descent_thrust_level));
        }
        break;

    case State::Release:
        // Reinitialise vertical position controller to remove discontinuity due to touch down of payload
        pos_control->init_U_controller_no_descent();
#if AP_GRIPPER_ENABLED
        if (AP::gripper().valid()) {
            gcs().send_text(MAV_SEVERITY_INFO, "%s Releasing the gripper", prefix_str);
            AP::gripper().release();
            state = State::Releasing;
        } else {
            state = State::Delay;
        }
#else
        state = State::Delay;
#endif
        break;

    case State::Releasing:
#if AP_GRIPPER_ENABLED
        if (AP::gripper().valid() && !AP::gripper().released()) {
            break;
        }
#endif
        state = State::Delay;
        FALLTHROUGH;

    case State::Delay:
        // If we get here we have finished releasing the gripper
        if (now_ms - place_start_time_ms < placed_check_duration_ms + g2.pldp_delay_s * 1000.0) {
            break;
        }
        FALLTHROUGH;

    case State::Ascent_Start:
        state = State::Ascent;
        FALLTHROUGH;

    case State::Ascent: {
        // Ascent complete when we are less than 10% of the stopping
        // distance from the target altitude stopping distance from
        // vel_threshold_fraction * max velocity
        const float vel_threshold_fraction = 0.1;
        const float stop_distance = 0.5 * sq(vel_threshold_fraction * copter.pos_control->get_max_speed_up_cms()) / copter.pos_control->get_max_accel_U_cmss();
        bool reached_altitude = pos_control->get_pos_desired_U_cm() >= descent_start_altitude_cm - stop_distance;
        if (reached_altitude) {
            state = State::Done;
        }
        break;
    }
    case State::Done:
        break;
    default:
        // this should never happen
        INTERNAL_ERROR(AP_InternalError::error_t::flow_of_control);
        break;
    }

    switch (state) {
    case State::FlyToLocation:
        // this should never happen
        return copter.mode_auto.wp_run();
    case State::Descent_Start:
    case State::Descent:
        copter.flightmode->land_run_horizontal_control();
        // update altitude target and call position controller
        pos_control->land_at_climb_rate_cm(-descent_speed_cms, true);
        break;
    case State::Release:
    case State::Releasing:
    case State::Delay:
    case State::Ascent_Start:
        copter.flightmode->land_run_horizontal_control();
        // update altitude target and call position controller
        pos_control->land_at_climb_rate_cm(0.0, false);
        break;
    case State::Ascent:
    case State::Done:
        float vel = 0.0;
        copter.flightmode->land_run_horizontal_control();
        pos_control->input_pos_vel_accel_U_cm(descent_start_altitude_cm, vel, 0.0);
        break;
    }
    pos_control->update_U_controller();
}
#endif

// sets the target_loc's alt to the vehicle's current alt but does not change target_loc's frame
// in the case of terrain altitudes either the terrain database or the rangefinder may be used
// returns true on success, false on failure
bool ModeAuto::shift_alt_to_current_alt(Location& target_loc) const
{
    // if terrain alt using rangefinder is being used then set alt to current rangefinder altitude
    if ((target_loc.get_alt_frame() == Location::AltFrame::ABOVE_TERRAIN) &&
        (wp_nav->get_terrain_source() == AC_WPNav::TerrainSource::TERRAIN_FROM_RANGEFINDER)) {
        int32_t curr_rngfnd_alt_cm;
        if (copter.get_rangefinder_height_interpolated_cm(curr_rngfnd_alt_cm)) {
            // subtract position offset (if any)
            curr_rngfnd_alt_cm -= pos_control->get_pos_offset_U_cm();
            // wp_nav is using rangefinder so use current rangefinder alt
            target_loc.set_alt_cm(MAX(curr_rngfnd_alt_cm, 200), Location::AltFrame::ABOVE_TERRAIN);
            return true;
        }
        return false;
    }

    // take copy of current location and change frame to match target
    Location currloc = copter.current_loc;
    if (!currloc.change_alt_frame(target_loc.get_alt_frame())) {
        // this could fail due missing terrain database alt
        return false;
    }

    // set target_loc's alt minus position offset (if any)
    target_loc.set_alt_cm(currloc.alt - pos_control->get_pos_offset_U_cm(), currloc.get_alt_frame());
    return true;
}

// subtract position controller offsets from target location
// should be used when the location will be used as a target for the position controller
void ModeAuto::subtract_pos_offsets(Location& target_loc) const
{
    // subtract position controller offsets from target location
    const Vector3p& pos_ofs_neu_cm = pos_control->get_pos_offset_NEU_cm();
    Vector3p pos_ofs_ned_m = Vector3p{pos_ofs_neu_cm.x * 0.01, pos_ofs_neu_cm.y * 0.01, -pos_ofs_neu_cm.z * 0.01};
    target_loc.offset(-pos_ofs_ned_m);
}

/********************************************************************************/
// Nav (Must) commands
/********************************************************************************/

// do_takeoff - initiate takeoff navigation command
void ModeAuto::do_takeoff(const AP_Mission::Mission_Command& cmd)
{
    // Set wp navigation target to safe altitude above current position
    takeoff_start(cmd.content.location);
}

// return the Location portion of a command.  If the command's lat and lon and/or alt are zero the default_loc's lat,lon and/or alt are returned instead
Location ModeAuto::loc_from_cmd(const AP_Mission::Mission_Command& cmd, const Location& default_loc) const
{
    Location ret(cmd.content.location);

    // use current lat, lon if zero
    if (ret.lat == 0 && ret.lng == 0) {
        ret.lat = default_loc.lat;
        ret.lng = default_loc.lng;
    }
    // use default altitude if not provided in cmd
    if (ret.alt == 0) {
        // set to default_loc's altitude but in command's alt frame
        // note that this may use the terrain database
        int32_t default_alt;
        if (default_loc.get_alt_cm(ret.get_alt_frame(), default_alt)) {
            ret.set_alt_cm(default_alt, ret.get_alt_frame());
        } else {
            // default to default_loc's altitude and frame
            ret.copy_alt_from(default_loc);
        }
    }
    return ret;
}

// do_nav_wp - initiate move to next waypoint
void ModeAuto::do_nav_wp(const AP_Mission::Mission_Command& cmd)
{
    // calculate default location used when lat, lon or alt is zero
    Location default_loc = copter.current_loc;

    // subtract position offsets
    subtract_pos_offsets(default_loc);

    if (wp_nav->is_active() && wp_nav->reached_wp_destination()) {
        if (!wp_nav->get_wp_destination_loc(default_loc)) {
            // this should never happen
            INTERNAL_ERROR(AP_InternalError::error_t::flow_of_control);
        }
    }

    // get waypoint's location from command and send to wp_nav
    const Location target_loc = loc_from_cmd(cmd, default_loc);

    if (!wp_start(target_loc)) {
        // failure to set next destination can only be because of missing terrain data
        copter.failsafe_terrain_on_event();
        return;
    }

    // this will be used to remember the time in millis after we reach or pass the WP.
    loiter_time = 0;
    // this is the delay, stored in seconds
    loiter_time_max = cmd.p1;

    // set next destination if necessary
    if (!set_next_wp(cmd, target_loc)) {
        // failure to set next destination can only be because of missing terrain data
        copter.failsafe_terrain_on_event();
        return;
    }
}

// checks the next mission command and adds it as a destination if necessary
// supports both straight line and spline waypoints
// cmd should be the current command
// default_loc should be the destination from the current_cmd but corrected for cases where user set lat, lon or alt to zero
// returns true on success, false on failure which should only happen due to a failure to retrieve terrain data
bool ModeAuto::set_next_wp(const AP_Mission::Mission_Command& current_cmd, const Location &default_loc)
{
    // do not add next wp if current command has a delay meaning the vehicle will stop at the destination
    if (current_cmd.p1 > 0) {
        return true;
    }

    // do not add next wp if there are no more navigation commands
    AP_Mission::Mission_Command next_cmd;
    if (!mission.get_next_nav_cmd(current_cmd.index+1, next_cmd)) {
        return true;
    }

    // whether vehicle should stop at the target position depends upon the next command
    switch (next_cmd.id) {
    case MAV_CMD_NAV_WAYPOINT:
    case MAV_CMD_NAV_LOITER_UNLIM:
#if AP_MISSION_NAV_PAYLOAD_PLACE_ENABLED
    case MAV_CMD_NAV_PAYLOAD_PLACE:
#endif
    case MAV_CMD_NAV_LOITER_TIME: {
        const Location dest_loc = loc_from_cmd(current_cmd, default_loc);
        const Location next_dest_loc = loc_from_cmd(next_cmd, dest_loc);
        return wp_nav->set_wp_destination_next_loc(next_dest_loc);
    }
    case MAV_CMD_NAV_SPLINE_WAYPOINT: {
        // get spline's location and next location from command and send to wp_nav
        Location next_dest_loc, next_next_dest_loc;
        bool next_next_dest_loc_is_spline;
        get_spline_from_cmd(next_cmd, default_loc, next_dest_loc, next_next_dest_loc, next_next_dest_loc_is_spline);
        return wp_nav->set_spline_destination_next_loc(next_dest_loc, next_next_dest_loc, next_next_dest_loc_is_spline);
    }
    case MAV_CMD_NAV_VTOL_LAND:
    case MAV_CMD_NAV_LAND:
        // stop because we may change between rel,abs and terrain alt types
    case MAV_CMD_NAV_LOITER_TURNS:
    case MAV_CMD_NAV_RETURN_TO_LAUNCH:
    case MAV_CMD_NAV_VTOL_TAKEOFF:
    case MAV_CMD_NAV_TAKEOFF:
        // always stop for RTL and takeoff commands
    default:
        // for unsupported commands it is safer to stop
        break;
    }

    return true;
}

// do_land - initiate landing procedure
void ModeAuto::do_land(const AP_Mission::Mission_Command& cmd)
{
    // To-Do: check if we have already landed

    // if location provided we fly to that location at current altitude
    if (cmd.content.location.lat != 0 || cmd.content.location.lng != 0) {
        // set state to fly to location
        state = State::FlyToLocation;

        // convert cmd to location class
        Location target_loc(cmd.content.location);
        if (!shift_alt_to_current_alt(target_loc)) {
            // this can only fail due to missing terrain database alt or rangefinder alt
            // use current alt-above-home and report error
            target_loc.set_alt_cm(copter.current_loc.alt, Location::AltFrame::ABOVE_HOME);
            LOGGER_WRITE_ERROR(LogErrorSubsystem::TERRAIN, LogErrorCode::MISSING_TERRAIN_DATA);
            gcs().send_text(MAV_SEVERITY_CRITICAL, "Land: no terrain data, using alt-above-home");
        }

        if (!wp_start(target_loc)) {
            // failure to set next destination can only be because of missing terrain data
            copter.failsafe_terrain_on_event();
            return;
        }
    } else {
        // set landing state
        state = State::Descending;

        // initialise landing controller
        land_start();
    }
}

// do_loiter_unlimited - start loitering with no end conditions
// note: caller should set yaw_mode
void ModeAuto::do_loiter_unlimited(const AP_Mission::Mission_Command& cmd)
{
    // calculate default location used when lat, lon or alt is zero
    Location default_loc = copter.current_loc;

    // subtract position offsets
    subtract_pos_offsets(default_loc);

    // use previous waypoint destination as default if available
    if (wp_nav->is_active() && wp_nav->reached_wp_destination()) {
        if (!wp_nav->get_wp_destination_loc(default_loc)) {
            // this should never happen
            INTERNAL_ERROR(AP_InternalError::error_t::flow_of_control);
        }
    }

    // get waypoint's location from command and send to wp_nav
    const Location target_loc = loc_from_cmd(cmd, default_loc);

    // start way point navigator and provide it the desired location
    if (!wp_start(target_loc)) {
        // failure to set next destination can only be because of missing terrain data
        copter.failsafe_terrain_on_event();
        return;
    }
}

// do_circle - initiate moving in a circle
void ModeAuto::do_circle(const AP_Mission::Mission_Command& cmd)
{
    // calculate default location used when lat, lon or alt is zero
    Location default_loc = copter.current_loc;

    // subtract position offsets
    subtract_pos_offsets(default_loc);

    const Location circle_center = loc_from_cmd(cmd, default_loc);

    // calculate radius
    uint16_t circle_radius_m = HIGHBYTE(cmd.p1); // circle radius held in high byte of p1
    if (cmd.id == MAV_CMD_NAV_LOITER_TURNS &&
        cmd.type_specific_bits & (1U << 0)) {
        // special storage handling allows for larger radii
        circle_radius_m *= 10;
    }

    // true if circle should be ccw
    const bool circle_direction_ccw = cmd.content.location.loiter_ccw;

    // move to edge of circle (verify_circle) will ensure we begin circling once we reach the edge
    circle_movetoedge_start(circle_center, circle_radius_m, circle_direction_ccw);

    circle_last_num_complete = -1;
}

// do_loiter_time - initiate loitering at a point for a given time period
// note: caller should set yaw_mode
void ModeAuto::do_loiter_time(const AP_Mission::Mission_Command& cmd)
{
    // re-use loiter unlimited
    do_loiter_unlimited(cmd);

    // setup loiter timer
    loiter_time     = 0;
    loiter_time_max = cmd.p1;     // units are (seconds)
}

// do_loiter_alt - initiate loitering at a point until a given altitude is reached
// note: caller should set yaw_mode
void ModeAuto::do_loiter_to_alt(const AP_Mission::Mission_Command& cmd)
{
    // re-use loiter unlimited
    do_loiter_unlimited(cmd);

    // if we aren't navigating to a location then we have to adjust
    // altitude for current location
    Location target_loc(cmd.content.location);
    if (target_loc.lat == 0 && target_loc.lng == 0) {
        target_loc.lat = copter.current_loc.lat;
        target_loc.lng = copter.current_loc.lng;
    }

    if (!target_loc.get_alt_cm(Location::AltFrame::ABOVE_HOME, loiter_to_alt.alt)) {
        loiter_to_alt.reached_destination_xy = true;
        loiter_to_alt.reached_alt = true;
        gcs().send_text(MAV_SEVERITY_INFO, "bad do_loiter_to_alt");
        return;
    }
    loiter_to_alt.reached_destination_xy = false;
    loiter_to_alt.loiter_start_done = false;
    loiter_to_alt.reached_alt = false;
    loiter_to_alt.alt_error_cm = 0;

    // set vertical speed and acceleration limits
    pos_control->set_max_speed_accel_U_cm(wp_nav->get_default_speed_down_cms(), wp_nav->get_default_speed_up_cms(), wp_nav->get_accel_U_cmss());
    pos_control->set_correction_speed_accel_U_cmss(wp_nav->get_default_speed_down_cms(), wp_nav->get_default_speed_up_cms(), wp_nav->get_accel_U_cmss());

    // set submode
    set_submode(SubMode::LOITER_TO_ALT);
}

// do_spline_wp - initiate move to next waypoint
void ModeAuto::do_spline_wp(const AP_Mission::Mission_Command& cmd)
{
    // calculate default location used when lat, lon or alt is zero
    Location default_loc = copter.current_loc;

    // subtract position offsets
    subtract_pos_offsets(default_loc);

    if (wp_nav->is_active() && wp_nav->reached_wp_destination()) {
        if (!wp_nav->get_wp_destination_loc(default_loc)) {
            // this should never happen
            INTERNAL_ERROR(AP_InternalError::error_t::flow_of_control);
        }
    }

    // get spline's location and next location from command and send to wp_nav
    Location dest_loc, next_dest_loc;
    bool next_dest_loc_is_spline;
    get_spline_from_cmd(cmd, default_loc, dest_loc, next_dest_loc, next_dest_loc_is_spline);
    if (!wp_nav->set_spline_destination_loc(dest_loc, next_dest_loc, next_dest_loc_is_spline)) {
        // failure to set destination can only be because of missing terrain data
        copter.failsafe_terrain_on_event();
        return;
    }

    // this will be used to remember the time in millis after we reach or pass the WP.
    loiter_time = 0;
    // this is the delay, stored in seconds
    loiter_time_max = cmd.p1;

    // set next destination if necessary
    if (!set_next_wp(cmd, dest_loc)) {
        // failure to set next destination can only be because of missing terrain data
        copter.failsafe_terrain_on_event();
        return;
    }

    // initialise yaw
    // To-Do: reset the yaw only when the previous navigation command is not a WP.  this would allow removing the special check for ROI
    if (auto_yaw.mode() != AutoYaw::Mode::ROI && !(auto_yaw.mode() == AutoYaw::Mode::FIXED && copter.g.wp_yaw_behavior == WP_YAW_BEHAVIOR_NONE)) {
        auto_yaw.set_mode_to_default(false);
    }

    // set submode
    set_submode(SubMode::WP);
}

// calculate locations required to build a spline curve from a mission command
// dest_loc is populated from cmd's location using default_loc in cases where the lat and lon or altitude is zero
// next_dest_loc and nest_dest_loc_is_spline is filled in with the following navigation command's location if it exists.  If it does not exist it is set to the dest_loc and false
void ModeAuto::get_spline_from_cmd(const AP_Mission::Mission_Command& cmd, const Location& default_loc, Location& dest_loc, Location& next_dest_loc, bool& next_dest_loc_is_spline)
{
    dest_loc = loc_from_cmd(cmd, default_loc);

    // if there is no delay at the end of this segment get next nav command
    AP_Mission::Mission_Command temp_cmd;
    if (cmd.p1 == 0 && mission.get_next_nav_cmd(cmd.index+1, temp_cmd)) {
        next_dest_loc = loc_from_cmd(temp_cmd, dest_loc);
        next_dest_loc_is_spline = temp_cmd.id == MAV_CMD_NAV_SPLINE_WAYPOINT;
    } else {
        next_dest_loc = dest_loc;
        next_dest_loc_is_spline = false;
    }
}

#if AC_NAV_GUIDED
// do_nav_guided_enable - initiate accepting commands from external nav computer
void ModeAuto::do_nav_guided_enable(const AP_Mission::Mission_Command& cmd)
{
    if (cmd.p1 > 0) {
        // start guided within auto
        nav_guided_start();
    }
}

// do_guided_limits - pass guided limits to guided controller
void ModeAuto::do_guided_limits(const AP_Mission::Mission_Command& cmd)
{
    copter.mode_guided.limit_set(
        cmd.p1 * 1000, // convert seconds to ms
        cmd.content.guided_limits.alt_min * 100.0f,    // convert meters to cm
        cmd.content.guided_limits.alt_max * 100.0f,    // convert meters to cm
        cmd.content.guided_limits.horiz_max * 100.0f); // convert meters to cm
}
#endif  // AC_NAV_GUIDED

// do_nav_delay - Delay the next navigation command
void ModeAuto::do_nav_delay(const AP_Mission::Mission_Command& cmd)
{
    nav_delay_time_start_ms = millis();

    if (cmd.content.nav_delay.seconds > 0) {
        // relative delay
        nav_delay_time_max_ms = cmd.content.nav_delay.seconds * 1000; // convert seconds to milliseconds
    } else {
        // absolute delay to utc time
#if AP_RTC_ENABLED
        nav_delay_time_max_ms = AP::rtc().get_time_utc(cmd.content.nav_delay.hour_utc, cmd.content.nav_delay.min_utc, cmd.content.nav_delay.sec_utc, 0);
#else
        nav_delay_time_max_ms = 0;
#endif
    }
    gcs().send_text(MAV_SEVERITY_INFO, "Delaying %u sec", (unsigned)(nav_delay_time_max_ms/1000));
}

#if AP_SCRIPTING_ENABLED
// start accepting position, velocity and acceleration targets from lua scripts
void ModeAuto::do_nav_script_time(const AP_Mission::Mission_Command& cmd)
{
    // call regular guided flight mode initialisation
    if (copter.mode_guided.init(true)) {
        nav_scripting.done = false;
        nav_scripting.id++;
        nav_scripting.start_ms = millis();
        nav_scripting.command = cmd.content.nav_script_time.command;
        nav_scripting.timeout_s = cmd.content.nav_script_time.timeout_s;
        nav_scripting.arg1 = cmd.content.nav_script_time.arg1.get();
        nav_scripting.arg2 = cmd.content.nav_script_time.arg2.get();
        nav_scripting.arg3 = cmd.content.nav_script_time.arg3;
        nav_scripting.arg4 = cmd.content.nav_script_time.arg4;
        set_submode(SubMode::NAV_SCRIPT_TIME);
    } else {
        // for safety we set nav_scripting to done to protect against the mission getting stuck
        nav_scripting.done = true;
    }
}
#endif

// start maintaining an attitude for a specified time
void ModeAuto::do_nav_attitude_time(const AP_Mission::Mission_Command& cmd)
{
    // copy command arguments into local structure
    nav_attitude_time.roll_deg = cmd.content.nav_attitude_time.roll_deg;
    nav_attitude_time.pitch_deg = cmd.content.nav_attitude_time.pitch_deg;
    nav_attitude_time.yaw_deg = cmd.content.nav_attitude_time.yaw_deg;
    nav_attitude_time.climb_rate = cmd.content.nav_attitude_time.climb_rate;
    nav_attitude_time.start_ms = AP_HAL::millis();
    set_submode(SubMode::NAV_ATTITUDE_TIME);
}

/********************************************************************************/
// Condition (May) commands
/********************************************************************************/

void ModeAuto::do_wait_delay(const AP_Mission::Mission_Command& cmd)
{
    condition_start = millis();
    condition_value = cmd.content.delay.seconds * 1000;     // convert seconds to milliseconds
}

void ModeAuto::do_within_distance(const AP_Mission::Mission_Command& cmd)
{
    condition_value  = cmd.content.distance.meters;
}

void ModeAuto::do_yaw(const AP_Mission::Mission_Command& cmd)
{
    auto_yaw.set_fixed_yaw_rad(
        radians(cmd.content.yaw.angle_deg),
        radians(cmd.content.yaw.turn_rate_dps),
        cmd.content.yaw.direction,
        cmd.content.yaw.relative_angle > 0);
}

/********************************************************************************/
// Do (Now) commands
/********************************************************************************/

void ModeAuto::do_change_speed(const AP_Mission::Mission_Command& cmd)
{
    if (cmd.content.speed.target_ms > 0) {
        switch (cmd.content.speed.speed_type) {
        case SPEED_TYPE_CLIMB_SPEED:
            copter.wp_nav->set_speed_up_cms(cmd.content.speed.target_ms * 100.0f);
            desired_speed_override.up = cmd.content.speed.target_ms;
            break;
        case SPEED_TYPE_DESCENT_SPEED:
            copter.wp_nav->set_speed_down_cms(cmd.content.speed.target_ms * 100.0f);
            desired_speed_override.down = cmd.content.speed.target_ms;
            break;
        case SPEED_TYPE_AIRSPEED:
        case SPEED_TYPE_GROUNDSPEED:
            copter.wp_nav->set_speed_NE_cms(cmd.content.speed.target_ms * 100.0f);
            desired_speed_override.xy = cmd.content.speed.target_ms;
            break;
        }
    }
}

void ModeAuto::do_set_home(const AP_Mission::Mission_Command& cmd)
{
    if (cmd.p1 == 1 || !cmd.content.location.initialised()) {
        if (!copter.set_home_to_current_location(false)) {
            // ignore failure
        }
    } else {
        if (!copter.set_home(cmd.content.location, false)) {
            // ignore failure
        }
    }
}

// do_roi - starts actions required by MAV_CMD_DO_SET_ROI
//          this involves either moving the camera to point at the ROI (region of interest)
//          and possibly rotating the copter to point at the ROI if our mount type does not support a yaw feature
// TO-DO: add support for other features of MAV_CMD_DO_SET_ROI including pointing at a given waypoint
void ModeAuto::do_roi(const AP_Mission::Mission_Command& cmd)
{
    auto_yaw.set_roi(cmd.content.location);
}

#if HAL_MOUNT_ENABLED
// point the camera to a specified angle
void ModeAuto::do_mount_control(const AP_Mission::Mission_Command& cmd)
{
    // if vehicle has a camera mount but it doesn't do pan control then yaw the entire vehicle instead
    if ((copter.camera_mount.get_mount_type() != AP_Mount::Type::None) &&
        !copter.camera_mount.has_pan_control()) {
        // Per the handler in AP_Mount, DO_MOUNT_CONTROL yaw angle is in body frame, which is
        // equivalent to an offset to the current yaw demand.
        auto_yaw.set_yaw_angle_offset_deg(cmd.content.mount_control.yaw);
    }
    // pass the target angles to the camera mount
    copter.camera_mount.set_angle_target(cmd.content.mount_control.roll, cmd.content.mount_control.pitch, cmd.content.mount_control.yaw, false);
}
#endif  // HAL_MOUNT_ENABLED

#if AP_WINCH_ENABLED
// control winch based on mission command
void ModeAuto::do_winch(const AP_Mission::Mission_Command& cmd)
{
    // Note: we ignore the gripper num parameter because we only support one gripper
    switch (cmd.content.winch.action) {
        case WINCH_RELAXED:
            g2.winch.relax();
            break;
        case WINCH_RELATIVE_LENGTH_CONTROL:
            g2.winch.release_length(cmd.content.winch.release_length);
            break;
        case WINCH_RATE_CONTROL:
            g2.winch.set_desired_rate(cmd.content.winch.release_rate);
            break;
        default:
            // do nothing
            break;
    }
}
#endif

#if AP_MISSION_NAV_PAYLOAD_PLACE_ENABLED && AC_PAYLOAD_PLACE_ENABLED
// do_payload_place - initiate placing procedure
void ModeAuto::do_payload_place(const AP_Mission::Mission_Command& cmd)
{
    // if location provided we fly to that location at current altitude
    if (cmd.content.location.lat != 0 || cmd.content.location.lng != 0) {
        // set state to fly to location
        payload_place.state = PayloadPlace::State::FlyToLocation;

        // convert cmd to location class
        Location target_loc(cmd.content.location);
        if (!shift_alt_to_current_alt(target_loc)) {
            // this can only fail due to missing terrain database alt or rangefinder alt
            // use current alt-above-home and report error
            target_loc.set_alt_cm(copter.current_loc.alt, Location::AltFrame::ABOVE_HOME);
            LOGGER_WRITE_ERROR(LogErrorSubsystem::TERRAIN, LogErrorCode::MISSING_TERRAIN_DATA);
            gcs().send_text(MAV_SEVERITY_CRITICAL, "PayloadPlace: no terrain data, using alt-above-home");
        }
        if (!wp_start(target_loc)) {
            // failure to set next destination can only be because of missing terrain data
            copter.failsafe_terrain_on_event();
            return;
        }
    } else {
        // initialise placing controller
        payload_place.start_descent();
    }
    payload_place.descent_max_cm = cmd.p1;

    // set submode
    set_submode(SubMode::NAV_PAYLOAD_PLACE);
}
#endif  // AP_MISSION_NAV_PAYLOAD_PLACE_ENABLED

// do_RTL - start Return-to-Launch
void ModeAuto::do_RTL(void)
{
    // start rtl in auto flight mode
    rtl_start();
}

/********************************************************************************/
// Verify Nav (Must) commands
/********************************************************************************/

// verify_takeoff - check if we have completed the takeoff
bool ModeAuto::verify_takeoff()
{
#if AP_LANDINGGEAR_ENABLED
    // if we have reached our destination
    if (auto_takeoff.complete) {
        // retract the landing gear
        copter.landinggear.retract_after_takeoff();
    }
#endif

    return auto_takeoff.complete;
}

// verify_land - returns true if landing has been completed
bool ModeAuto::verify_land()
{
    bool retval = false;

    switch (state) {
        case State::FlyToLocation:
            // check if we've reached the location
            if (copter.wp_nav->reached_wp_destination()) {
                // initialise landing controller
                land_start();

                // advance to next state
                state = State::Descending;
            }
            break;

        case State::Descending:
            // rely on THROTTLE_LAND mode to correctly update landing status
            retval = copter.ap.land_complete && (motors->get_spool_state() == AP_Motors::SpoolState::GROUND_IDLE);
            if (retval && !mission.continue_after_land_check_for_takeoff() && copter.motors->armed()) {
                /*
                  we want to stop mission processing on land
                  completion. Disarm now, then return false. This
                  leaves mission state machine in the current NAV_LAND
                  mission item. After disarming the mission will reset
                */
                copter.arming.disarm(AP_Arming::Method::LANDED);
                retval = false;
            }
            break;

        default:
            // this should never happen
            INTERNAL_ERROR(AP_InternalError::error_t::flow_of_control);
            retval = true;
            break;
    }

    // true is returned if we've successfully landed
    return retval;
}

#if AC_PAYLOAD_PLACE_ENABLED
// verify_payload_place - returns true if placing has been completed
bool PayloadPlace::verify()
{
    switch (state) {
    case State::FlyToLocation:
    case State::Descent_Start:
    case State::Descent:
    case State::Release:
    case State::Releasing:
    case State::Delay:
    case State::Ascent_Start:
    case State::Ascent:
        return false;
    case State::Done:
        return true;
    }
    // should never get here
    return true;
}
#endif

bool ModeAuto::verify_loiter_unlimited()
{
    return false;
}

// verify_loiter_time - check if we have loitered long enough
bool ModeAuto::verify_loiter_time(const AP_Mission::Mission_Command& cmd)
{
    // return immediately if we haven't reached our destination
    if (!copter.wp_nav->reached_wp_destination()) {
        return false;
    }

    // start our loiter timer
    if ( loiter_time == 0 ) {
        loiter_time = millis();
    }

    // check if loiter timer has run out
    if (((millis() - loiter_time) / 1000) >= loiter_time_max) {
        gcs().send_text(MAV_SEVERITY_INFO, "Reached command #%i",cmd.index);
        return true;
    }

    return false;
}

// verify_loiter_to_alt - check if we have reached both destination
// (roughly) and altitude (precisely)
bool ModeAuto::verify_loiter_to_alt() const
{
    if (loiter_to_alt.reached_destination_xy &&
        loiter_to_alt.reached_alt) {
        return true;
    }
    return false;
}

// verify_RTL - handles any state changes required to implement RTL
// do_RTL should have been called once first to initialise all variables
// returns true with RTL has completed successfully
bool ModeAuto::verify_RTL()
{
    return (copter.mode_rtl.state_complete() && 
            (copter.mode_rtl.state() == ModeRTL::SubMode::FINAL_DESCENT || copter.mode_rtl.state() == ModeRTL::SubMode::LAND) &&
            (motors->get_spool_state() == AP_Motors::SpoolState::GROUND_IDLE));
}

/********************************************************************************/
// Verify Condition (May) commands
/********************************************************************************/

bool ModeAuto::verify_wait_delay()
{
    if (millis() - condition_start > (uint32_t)MAX(condition_value,0)) {
        condition_value = 0;
        return true;
    }
    return false;
}

bool ModeAuto::verify_within_distance()
{
    if (wp_distance_m() < MAX(condition_value,0)) {
        condition_value = 0;
        return true;
    }
    return false;
}

// verify_yaw - return true if we have reached the desired heading
bool ModeAuto::verify_yaw()
{
    // make sure still in fixed yaw mode, the waypoint controller often retakes control of yaw as it executes a new waypoint command
    auto_yaw.set_mode(AutoYaw::Mode::FIXED);

    // check if we have reached the target heading
    return auto_yaw.reached_fixed_yaw_target();
}

// verify_nav_wp - check if we have reached the next way point
bool ModeAuto::verify_nav_wp(const AP_Mission::Mission_Command& cmd)
{
    // check if we have reached the waypoint
    if ( !copter.wp_nav->reached_wp_destination() ) {
        return false;
    }

    // start timer if necessary
    if (loiter_time == 0) {
        loiter_time = millis();
        if (loiter_time_max > 0) {
            // play a tone
            AP_Notify::events.waypoint_complete = 1;
        }
    }

    // check if timer has run out
    if (((millis() - loiter_time) / 1000) >= loiter_time_max) {
        if (loiter_time_max == 0) {
            // play a tone
            AP_Notify::events.waypoint_complete = 1;
        }
        gcs().send_text(MAV_SEVERITY_INFO, "Reached command #%i",cmd.index);
        return true;
    }
    return false;
}

// verify_circle - check if we have circled the point enough
bool ModeAuto::verify_circle(const AP_Mission::Mission_Command& cmd)
{
    // check if we've reached the edge
    if (_mode == SubMode::CIRCLE_MOVE_TO_EDGE) {
        if (copter.wp_nav->reached_wp_destination()) {
            // start circling
            circle_start();
        }
        return false;
    }

    const float turns = cmd.get_loiter_turns();

    const auto num_circles_completed = fabsf(copter.circle_nav->get_angle_total_rad()/float(M_2PI));
    if (int(num_circles_completed) != int(circle_last_num_complete)) {
        circle_last_num_complete = num_circles_completed;
        GCS_SEND_TEXT(MAV_SEVERITY_INFO, "Mission: starting circle %u/%u", unsigned(num_circles_completed)+1, unsigned(turns));
    }

    // check if we have completed circling
    return num_circles_completed >= turns;
}

// verify_spline_wp - check if we have reached the next way point using spline
bool ModeAuto::verify_spline_wp(const AP_Mission::Mission_Command& cmd)
{
    // check if we have reached the waypoint
    if ( !copter.wp_nav->reached_wp_destination() ) {
        return false;
    }

    // start timer if necessary
    if (loiter_time == 0) {
        loiter_time = millis();
    }

    // check if timer has run out
    if (((millis() - loiter_time) / 1000) >= loiter_time_max) {
        gcs().send_text(MAV_SEVERITY_INFO, "Reached command #%i",cmd.index);
        return true;
    }
    return false;
}

#if AC_NAV_GUIDED
// verify_nav_guided - check if we have breached any limits
bool ModeAuto::verify_nav_guided_enable(const AP_Mission::Mission_Command& cmd)
{
    // if disabling guided mode then immediately return true so we move to next command
    if (cmd.p1 == 0) {
        return true;
    }

    // check time and position limits
    return copter.mode_guided.limit_check();
}
#endif  // AC_NAV_GUIDED

// verify_nav_delay - check if we have waited long enough
bool ModeAuto::verify_nav_delay(const AP_Mission::Mission_Command& cmd)
{
    if (millis() - nav_delay_time_start_ms > nav_delay_time_max_ms) {
        nav_delay_time_max_ms = 0;
        return true;
    }
    return false;
}

#if AP_SCRIPTING_ENABLED
// check if verify_nav_script_time command has completed
bool ModeAuto::verify_nav_script_time()
{
    // if done or timeout then return true
    if (nav_scripting.done ||
        ((nav_scripting.timeout_s > 0) &&
         (AP_HAL::millis() - nav_scripting.start_ms) > (nav_scripting.timeout_s * 1000))) {
        return true;
    }
    return false;
}
#endif

// check if nav_attitude_time command has completed
bool ModeAuto::verify_nav_attitude_time(const AP_Mission::Mission_Command& cmd)
{
    return ((AP_HAL::millis() - nav_attitude_time.start_ms) > (cmd.content.nav_attitude_time.time_sec * 1000));
}

// pause - Prevent aircraft from progressing along the track
bool ModeAuto::pause()
{
    // do not pause if not in the WP sub mode or already reached to the destination
    if (_mode != SubMode::WP || wp_nav->reached_wp_destination()) {
        return false;
    }

    wp_nav->set_pause();
    return true;
}

// resume - Allow aircraft to progress along the track
bool ModeAuto::resume()
{
    wp_nav->set_resume();
    return true;
}

bool ModeAuto::paused() const
{
    return (wp_nav != nullptr) && wp_nav->paused();
}

/*
  get a height above ground estimate for landing
 */
int32_t ModeAuto::get_alt_above_ground_cm() const
{
    // Only override if in landing submode
    if (_mode == SubMode::LAND) {
        // Rangefinder takes priority
        int32_t alt_above_ground_cm;
        if (copter.get_rangefinder_height_interpolated_cm(alt_above_ground_cm)) {
            return alt_above_ground_cm;
        }

        // Take land altitude from command
        const AP_Mission::Mission_Command& cmd = mission.get_current_nav_cmd();
        switch (cmd.id) {
        case MAV_CMD_NAV_VTOL_LAND:
        case MAV_CMD_NAV_LAND: {
            if (cmd.content.location.lat != 0 || cmd.content.location.lng != 0) {
                // If land location is valid return height above it
                ftype dist;
                if (copter.current_loc.get_height_above(cmd.content.location, dist)) {
                    return dist * 100.0;
                }
            }
            break;
        }

        default:
            // Really should not end up here as were in SubMode land
            break;
        }
    }

    // Use default method
    return Mode::get_alt_above_ground_cm();
}

#endif
