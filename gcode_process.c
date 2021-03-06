#include	"gcode_process.h"

/** \file
	\brief Work out what to do with received G-Code commands
*/

#include	<string.h>

#include	"gcode_parse.h"

#include "cpu.h"
#include	"dda.h"
#include	"dda_maths.h"
#include	"dda_queue.h"
#include	"watchdog.h"
#include	"delay.h"
#include	"serial.h"
#include	"temp.h"
#include	"heater.h"
#include	"timer.h"
#include	"sersendf.h"
#include	"pinio.h"
#include	"debug.h"
#include	"clock.h"
#include	"config_wrapper.h"
#include	"home.h"
#include "sd.h"


/// the current tool
uint8_t tool;
uint8_t qretract;
/// the tool to be changed when we get an M6
uint8_t next_tool;
extern axes_uint32_t  maximum_feedrate_P;
/************************************************************************//**

  \brief Processes command stored in global \ref next_target.
  This is where we work out what to actually do with each command we
    receive. All data has already been scaled to integers in gcode_process.
    If you want to add support for a new G or M code, this is the place.


*//*************************************************************************/

int32_t lastE,lastZ;
int32_t EEMEM EE_retract;
int32_t EEMEM EE_retractZ;
int32_t EEMEM EE_retractF;
int8_t inretract;


void process_gcode_command() {
	uint32_t	backup_f;

	// convert relative to absolute
	if (next_target.option_all_relative) {
    next_target.target.axis[X] += startpoint.axis[X];
    next_target.target.axis[Y] += startpoint.axis[Y];
    next_target.target.axis[Z] += startpoint.axis[Z];
	}

	// E relative movement.
	// Matches Sprinter's behaviour as of March 2012.
	if (next_target.option_all_relative || next_target.option_e_relative)
		next_target.target.e_relative = 1;
	else
		next_target.target.e_relative = 0;

	// implement axis limits
	#ifdef	X_MIN
    if (next_target.target.axis[X] < (int32_t)(X_MIN * 1000.))
      next_target.target.axis[X] = (int32_t)(X_MIN * 1000.);
	#endif
	#ifdef	X_MAX
    if (next_target.target.axis[X] > (int32_t)(X_MAX * 1000.))
      next_target.target.axis[X] = (int32_t)(X_MAX * 1000.);
	#endif
	#ifdef	Y_MIN
    if (next_target.target.axis[Y] < (int32_t)(Y_MIN * 1000.))
      next_target.target.axis[Y] = (int32_t)(Y_MIN * 1000.);
	#endif
	#ifdef	Y_MAX
    if (next_target.target.axis[Y] > (int32_t)(Y_MAX * 1000.))
      next_target.target.axis[Y] = (int32_t)(Y_MAX * 1000.);
	#endif
	#ifdef	Z_MIN
    if (next_target.target.axis[Z] < (int32_t)(Z_MIN * 1000.))
      next_target.target.axis[Z] = (int32_t)(Z_MIN * 1000.);
	#endif
	#ifdef	Z_MAX
    int32_t zmax=eeprom_read_dword ((uint32_t *) &EE_real_zmax);
    if (next_target.target.axis[Z] > (int32_t)(zmax))
      next_target.target.axis[Z] = (int32_t)(zmax);
	#endif

	// The GCode documentation was taken from http://reprap.org/wiki/Gcode .

	if (next_target.seen_T) {
	    //? --- T: Select Tool ---
	    //?
	    //? Example: T1
	    //?
	    //? Select extruder number 1 to build with.  Extruder numbering starts at 0.

	    next_tool = next_target.T;
	}

	if (next_target.seen_G) {
		uint8_t axisSelected = 0;
        //sersendf_P(PSTR("Gcode %su \n"),next_target.G);
		switch (next_target.G) {
			case 0:
				//? G0: Rapid Linear Motion
				//?
				//? Example: G0 X12
				//?
				//? In this case move rapidly to X = 12 mm.  In fact, the RepRap firmware uses exactly the same code for rapid as it uses for controlled moves (see G1 below), as - for the RepRap machine - this is just as efficient as not doing so.  (The distinction comes from some old machine tools that used to move faster if the axes were not driven in a straight line.  For them G0 allowed any movement in space to get to the destination as fast as possible.)
				//?
        temp_wait();
                if (!next_target.seen_F) {
				//backup_f = next_target.target.F;
				//next_target.target.F = maximum_feedrate_P[X] * 2L;
                //next_target.target.axis[E]=0;
                if (next_target.target.F>maximum_feedrate_P[X]) next_target.target.F=maximum_feedrate_P[X];
				enqueue(&next_target.target);
				//next_target.target.F = backup_f;
				} else enqueue(&next_target.target);
                break;

			case 1:
				//? --- G1: Linear Motion at Feed Rate ---
				//?
				//? Example: G1 X90.6 Y13.8 E22.4
				//?
				//? Go in a straight line from the current (X, Y) point to the point (90.6, 13.8), extruding material as the move happens from the current extruded length to a length of 22.4 mm.
				//?
        temp_wait();
                //next_target.target.axis[E]=0;
                // auto retraction change
                uint32_t retr=eeprom_read_dword((uint32_t *) &EE_retract);
                if (retr && !next_target.option_all_relative) {
                    if (next_target.seen_E){
                        if ( next_target.seen_F  && lastE && (next_target.target.axis[E]<lastE) && !(next_target.seen_X || next_target.seen_Y || next_target.seen_Z )){
                                // retract
                                next_target.target.axis[E]=lastE - retr; 
                                next_target.target.F=eeprom_read_dword((uint32_t *) &EE_retractF);
                                inretract=1;
                        } else {
                            lastE=next_target.target.axis[E];
                            //inretract=0;
                        }
                    }
                    // auto z hop  change
                    if (next_target.seen_Z) {
                        if (inretract && (next_target.target.axis[Z]>lastZ)){
                            next_target.target.axis[Z]=lastZ + eeprom_read_dword((uint32_t *) &EE_retractZ); 
                            inretract=0;
                        } else {
                            lastZ=next_target.target.axis[Z];
                        }
                        
                    } 
                } 
                
				if (next_target.target.F>maximum_feedrate_P[X]) next_target.target.F=maximum_feedrate_P[X];
				enqueue(&next_target.target);
				break;

				//	G2 - Arc Clockwise
				//	G3 - Arc anti Clockwise
            case 2:
            case 3:
                // we havent immplement R
                ///*
#ifdef ARC_SUPPORT                
                temp_wait();
                
                if (!next_target.seen_I) next_target.I=0;
                if (!next_target.seen_J) next_target.J=0;
                //if (DEBUG_ECHO && (debug_flags & DEBUG_ECHO))
                    
                draw_arc(&next_target.target,next_target.I,next_target.J, next_target.G==2);
#endif                
                 //*/
                break;

			case 4:
				//? --- G4: Dwell ---
				//?
				//? Example: G4 P200
				//?
				//? In this case sit still doing nothing for 200 milliseconds.  During delays the state of the machine (for example the temperatures of its extruders) will still be preserved and controlled.
				//?
				queue_wait();
				// delay
				if (next_target.seen_P) {
					for (;next_target.P > 0;next_target.P--) {
						clock();
						delay_ms(1);
					}
				}
				break;
            #ifdef INCH_SUPPORT
					
			case 20:
				//? --- G20: Set Units to Inches ---
				//?
				//? Example: G20
				//?
				//? Units from now on are in inches.
				//?
				next_target.option_inches = 1;
				break;

			case 21:
				//? --- G21: Set Units to Millimeters ---
				//?
				//? Example: G21
				//?
				//? Units from now on are in millimeters.  (This is the RepRap default.)
				//?
				next_target.option_inches = 0;
				break;
            #endif
			case 28:
				//? --- G28: Home ---
				//?
				//? Example: G28
				//?
        //? This causes the RepRap machine to search for its X, Y and Z
        //? endstops. It does so at high speed, so as to get there fast. When
        //? it arrives it backs off slowly until the endstop is released again.
        //? Backing off slowly ensures more accurate positioning.
				//?
        //? If you add axis characters, then just the axes specified will be
        //? seached. Thus
				//?
        //?   G28 X Y72.3
				//?
        //? will zero the X and Y axes, but not Z. Coordinate values are
        //? ignored.
				//?

				queue_wait();

				if (next_target.seen_X) {
					next_target.target.axis[X] =0;
                    #if defined	X_MIN_PIN
						home_x_negative();
					#elif defined X_MAX_PIN
						home_x_positive();
					#endif
					axisSelected = 1;
				}
				if (next_target.seen_Y) {
					next_target.target.axis[Y] =0;
                    #if defined	Y_MIN_PIN
						home_y_negative();
					#elif defined Y_MAX_PIN
						home_y_positive();
					#endif
					axisSelected = 1;
				}
				if (next_target.seen_Z) {
                    //next_target.target.axis[Z] =0;
                      #if defined Z_MIN_PIN
                        home_z_negative();
                      #elif defined Z_MAX_PIN
                        home_z_positive();
                    #endif
					axisSelected = 1;
				}
				// there's no point in moving E, as E has no endstops

				if (!axisSelected) {
                    temp_wait();
                     #ifdef DELTA_PRINTER
                     home();
                     #else
				backup_f = next_target.target.F;
				next_target.target.F = MAXIMUM_FEEDRATE_X * 2L;
				next_target.target.axis[X] =
                next_target.target.axis[Y] =0;
                //next_target.target.axis[Z] =0;                
 
                enqueue(&next_target.target);
				next_target.target.F = backup_f;
					
                    home();
                    startpoint.axis[E] = next_target.target.axis[E] = 0;
                    dda_new_startpoint();
                    #endif
				}
                update_current_position();
				sersendf_P(PSTR("X: %lq Y: %lq Z: %lq E: %lq\n"),  //F:%lu\n"
                        current_position.axis[X], current_position.axis[Y],
                        current_position.axis[Z], current_position.axis[E]);//,current_position.F);
                
                qretract=0;
				break;

			case 90:
				//? --- G90: Set to Absolute Positioning ---
				//?
				//? Example: G90
				//?
				//? All coordinates from now on are absolute relative to the origin
				//? of the machine. This is the RepRap default.
				//?
				//? If you ever want to switch back and forth between relative and
				//? absolute movement keep in mind, X, Y and Z follow the machine's
				//? coordinate system while E doesn't change it's position in the
				//? coordinate system on relative movements.
				//?

				// No wait_queue() needed.
				next_target.option_all_relative = 0;
				break;

			case 91:
				//? --- G91: Set to Relative Positioning ---
				//?
				//? Example: G91
				//?
				//? All coordinates from now on are relative to the last position.
				//?

				// No wait_queue() needed.
				next_target.option_all_relative = 1;
				break;

			case 92:
				//? --- G92: Set Position ---
				//?
				//? Example: G92 X10 E90
				//?
				//? Allows programming of absolute zero point, by reseting the current position to the values specified.  This would set the machine's X coordinate to 10, and the extrude coordinate to 90. No physical motion will occur.
				//?

				queue_wait();

				if (next_target.seen_X) {
          startpoint.axis[X] = next_target.target.axis[X];
					axisSelected = 1;
				}
				if (next_target.seen_Y) {
          startpoint.axis[Y] = next_target.target.axis[Y];
					axisSelected = 1;
				}
				if (next_target.seen_Z) {
          startpoint.axis[Z] = next_target.target.axis[Z];
					axisSelected = 1;
				}
				if (next_target.seen_E) {
          lastE=startpoint.axis[E] = next_target.target.axis[E];
                    
					axisSelected = 1;
				}

				if (axisSelected == 0) {
          startpoint.axis[X] = next_target.target.axis[X] =
          startpoint.axis[Y] = next_target.target.axis[Y] =
          startpoint.axis[Z] = next_target.target.axis[Z] =
          startpoint.axis[E] = next_target.target.axis[E] = 0;
				}

				dda_new_startpoint();
				break;

			case 161:
				//? --- G161: Home negative ---
				//?
				//? Find the minimum limit of the specified axes by searching for the limit switch.
				//?
        #if defined X_MIN_PIN
          if (next_target.seen_X)
            home_x_negative();
        #endif
        #if defined Y_MIN_PIN
          if (next_target.seen_Y)
            home_y_negative();
        #endif
        #if defined Z_MIN_PIN
          if (next_target.seen_Z)
            home_z_negative();
        #endif
				break;

			case 162:
				//? --- G162: Home positive ---
				//?
				//? Find the maximum limit of the specified axes by searching for the limit switch.
				//?
        #if defined X_MAX_PIN
          if (next_target.seen_X)
            home_x_positive();
        #endif
        #if defined Y_MAX_PIN
          if (next_target.seen_Y)
            home_y_positive();
        #endif
        #if defined Z_MAX_PIN
          if (next_target.seen_Z)
            home_z_positive();
        #endif
				break;

				// unknown gcode: spit an error
			default:
				sersendf_P(PSTR("E: Bad G-code %d\n"), next_target.G);
				return;
		}
	}
	else if (next_target.seen_M) {
		//uint8_t i;

		switch (next_target.M) {
			case 0:
				//? --- M0: machine stop ---
				//?
				//? Example: M0
				//?
				//? http://linuxcnc.org/handbook/RS274NGC_3/RS274NGC_33a.html#1002379
				//? Unimplemented, especially the restart after the stop. Fall trough to M2.
				//?

			case 2:
      case 84: // For compatibility with slic3rs default end G-code.
				//? --- M2: program end ---
				//?
				//? Example: M2
				//?
				//? http://linuxcnc.org/handbook/RS274NGC_3/RS274NGC_33a.html#1002379
				//?
				queue_wait();
				//for (i = 0; i < NUM_HEATERS; i++)temp_set(i, 0);
				power_off();
        serial_writestr_P(PSTR("\nstop\n"));
				break;

			case 6:
				//? --- M6: tool change ---
				//?
				//? Undocumented.
				tool = next_tool;
				break;

      #ifdef SD
      case 20:
        //? --- M20: list SD card. ---
        sd_list("/");
        break;

      case 21:
        //? --- M21: initialise SD card. ---
        //?
        //? Has to be done before doing any other operation, including M20.
        sd_mount();
        break;

      case 22:
        //? --- M22: release SD card. ---
        //?
        //? Not mandatory. Just removing the card is fine, but results in
        //? odd behaviour when trying to read from the card anyways. M22
        //? makes also sure SD card printing is disabled, even with the card
        //? inserted.
        sd_unmount();
        break;

      case 23:
        //? --- M23: select file. ---
        //?
        //? This opens a file for reading. This file is valid up to M22 or up
        //? to the next M23.
        sd_open(gcode_str_buf);
        break;

      case 24:
        //? --- M24: start/resume SD print. ---
        //?
        //? This makes the SD card available as a G-code source. File is the
        //? one selected with M23.
        gcode_sources |= GCODE_SOURCE_SD;
        break;

      case 25:
        //? --- M25: pause SD print. ---
        //?
        //? This removes the SD card from the bitfield of available G-code
        //? sources. The file is kept open. The position inside the file
        //? is kept as well, to allow resuming.
        gcode_sources &= ! GCODE_SOURCE_SD;
        break;
      #endif /* SD */
            case 209:
                    if (next_target.seen_S) qretract=next_target.S;
			case 82:
				//? --- M82 - Set E codes absolute ---
				//?
				//? This is the default and overrides G90/G91.
				//? M82/M83 is not documented in the RepRap wiki, behaviour
				//? was taken from Sprinter as of March 2012.
				//?
				//? While E does relative movements, it doesn't change its
				//? position in the coordinate system. See also comment on G90.
				//?

				// No wait_queue() needed.
				next_target.option_e_relative = 0;
				break;

			case 83:
				//? --- M83 - Set E codes relative ---
				//?
				//? Counterpart to M82.
				//?

				// No wait_queue() needed.
				next_target.option_e_relative = 1;
				break;

			// M3/M101- extruder on
			case 3:
			case 101:
				//? --- M101: extruder on ---
				//?
				//? Undocumented.
        temp_wait();
				#ifdef DC_EXTRUDER
					heater_set(DC_EXTRUDER, DC_EXTRUDER_PWM);
				#endif
				break;

			// M5/M103- extruder off
			case 5:
			case 103:
				//? --- M103: extruder off ---
				//?
				//? Undocumented.
				#ifdef DC_EXTRUDER
					heater_set(DC_EXTRUDER, 0);
				#endif
				break;

			case 104:
				//? --- M104: Set Extruder Temperature (Fast) ---
				//?
				//? Example: M104 S190
				//?
        //? Set the temperature of the current extruder to 190<sup>o</sup>C
        //? and return control to the host immediately (''i.e.'' before that
        //? temperature has been reached by the extruder). For waiting, see M116.
        //?
        //? Teacup supports an optional P parameter as a zero-based temperature
        //? sensor index to address (e.g. M104 P1 S100 will set the temperature
        //? of the heater connected to the second temperature sensor rather
        //? than the extruder temperature).
        //?
				if ( ! next_target.seen_S)
					break;
        if ( ! next_target.seen_P)
          #ifdef HEATER_EXTRUDER
            next_target.P = HEATER_EXTRUDER;
          #else
            next_target.P = 0;
          #endif
				temp_set(next_target.P, next_target.S);
				break;

			case 105:
        //? --- M105: Get Temperature(s) ---
				//?
				//? Example: M105
				//?
        //? Request the temperature of the current extruder and the build base
        //? in degrees Celsius. For example, the line sent to the host in
        //? response to this command looks like
				//?
				//? <tt>ok T:201 B:117</tt>
				//?
        //? Teacup supports an optional P parameter as a zero-based temperature
        //? sensor index to address.
				//?
				#ifdef ENFORCE_ORDER
					queue_wait();
				#endif
				if ( ! next_target.seen_P)
					next_target.P = TEMP_SENSOR_none;
				temp_print(next_target.P);
                sersendf_P(PSTR("FlowMultiply: %d\n"), (next_target.target.e_multiplier*100/256));
                sersendf_P(PSTR("SpeedMultiply: %d\n"), (next_target.target.f_multiplier*100/256));
                //print_queue();
                //queue_len();
                //sersendf_P(PSTR("Buffer: %d\n"), (mb_ctr));
                
				break;

			case 7:
            case 107:
                heater_set(HEATER_FAN, 128);
			case 106:
				//? --- M106: Set Fan Speed / Set Device Power ---
				//?
				//? Example: M106 S120
				//?
				//? Control the cooling fan (if any).
				//?
        //? Teacup supports an optional P parameter as a zero-based heater
        //? index to address. The heater index can differ from the temperature
        //? sensor index, see config.h.

				#ifdef ENFORCE_ORDER
					// wait for all moves to complete
					queue_wait();
				#endif
        if ( ! next_target.seen_P)
          #ifdef HEATER_FAN
            next_target.P = HEATER_FAN;
          #else
            next_target.P = 0;
          #endif
				if ( ! next_target.seen_S)
					break;
        heater_set(next_target.P, next_target.S);
				break;
			case 109:
				//? --- M116: Wait ---
				//?
				//? Example: M116
				//?
				//? Wait for temperatures and other slowly-changing variables to arrive at their set values.
                if ( ! next_target.seen_S)
					break;
        if ( ! next_target.seen_P)
          #ifdef HEATER_EXTRUDER
            next_target.P = HEATER_EXTRUDER;
          #else
            next_target.P = 0;
          #endif
                uint32_t adjt=eeprom_read_dword((uint32_t *) &EE_adjust_temp)*4;
                if ((adjt<-70*4) && (adjt>70*4)) adjt=0; 
				temp_set(next_target.P, next_target.S+adjt);
       temp_set_wait();
				break;
			case 110:
				//? --- M110: Set Current Line Number ---
				//?
				//? Example: N123 M110
				//?
				//? Set the current line number to 123.  Thus the expected next line after this command will be 124.
				//? This is a no-op in Teacup.
				//?
				break;

      #ifdef DEBUG
			case 111:
				//? --- M111: Set Debug Level ---
				//?
				//? Example: M111 S6
				//?
				//? Set the level of debugging information transmitted back to the host to level 6.  The level is the OR of three bits:
				//?
				//? <Pre>
				//? #define         DEBUG_PID       1
				//? #define         DEBUG_DDA       2
				//? #define         DEBUG_POSITION  4
				//? </pre>
				//?
				//? This command is only available in DEBUG builds of Teacup.

				if ( ! next_target.seen_S)
					break;
				debug_flags = next_target.S;
				break;
      #endif /* DEBUG */

      case 112:
        //? --- M112: Emergency Stop ---
        //?
        //? Example: M112
        //?
        //? Any moves in progress are immediately terminated, then the printer
        //? shuts down. All motors and heaters are turned off. Only way to
        //? restart is to press the reset button on the master microcontroller.
        //? See also M0.
        //?
        timer_stop();
        queue_flush();
        power_off();
        cli();
        for (;;)
          wd_reset();
        break;

			case 114:
				//? --- M114: Get Current Position ---
				//?
				//? Example: M114
				//?
				//? This causes the RepRap machine to report its current X, Y, Z and E coordinates to the host.
				//?
				//? For example, the machine returns a string such as:
				//?
				//? <tt>ok C: X:0.00 Y:0.00 Z:0.00 E:0.00</tt>
				//?
				#ifdef ENFORCE_ORDER
					// wait for all moves to complete
					queue_wait();
				#endif
				update_current_position();
				sersendf_P(PSTR("X:%lq Y:%lq Z:%lq E:%lq F:%lu\n"),
                        current_position.axis[X], current_position.axis[Y],
                        current_position.axis[Z], current_position.axis[E],
				                current_position.F);

        if (mb_tail_dda != NULL) {
          if (DEBUG_POSITION && (debug_flags & DEBUG_POSITION)) {
            sersendf_P(PSTR("Endpoint: X:%ld,Y:%ld,Z:%ld,E:%ld,F:%lu,c:%lu}\n"),
                       mb_tail_dda->endpoint.axis[X],
                       mb_tail_dda->endpoint.axis[Y],
                       mb_tail_dda->endpoint.axis[Z],
                       mb_tail_dda->endpoint.axis[E],
                       mb_tail_dda->endpoint.F,
                       #ifdef ACCELERATION_REPRAP
                         mb_tail_dda->end_c
                       #else
                         mb_tail_dda->c
                       #endif
            );
          }
          print_queue();
        }

				break;

			case 115:
				//? --- M115: Get Firmware Version and Capabilities ---
				//?
				//? Example: M115
				//?
				//? Request the Firmware Version and Capabilities of the current microcontroller
				//? The details are returned to the host computer as key:value pairs separated by spaces and terminated with a linefeed.
				//?
				//? sample data from firmware:
				//?  FIRMWARE_NAME:Teacup FIRMWARE_URL:http://github.com/traumflug/Teacup_Firmware/ PROTOCOL_VERSION:1.0 MACHINE_TYPE:Mendel EXTRUDER_COUNT:1 TEMP_SENSOR_COUNT:1 HEATER_COUNT:1
				//?

				//sersendf_P(PSTR("FIRMWARE_NAME:Teacup FIRMWARE_URL:http://github.com/traumflug/Teacup_Firmware/ PROTOCOL_VERSION:1.0 MACHINE_TYPE:Mendel EXTRUDER_COUNT:%d TEMP_SENSOR_COUNT:%d HEATER_COUNT:%d\n"), 1, NUM_TEMP_SENSORS, NUM_HEATERS);
///*
#ifdef KINEMATICS_DELTA
#define MACHINE_TYPE "Delta"
#endif
#ifdef KINEMATICS_STRAIGHT
#define MACHINE_TYPE "Cartesian"
#endif
#ifdef KINEMATICS_COREXY
#define MACHINE_TYPE "Corexy"
#endif

#define FIRMWARE_URL "https://github.com/repetier/Repetier-Firmware/"

                sersendf_P(PSTR("FIRMWARE_NAME:Repetier_1.9 FIRMWARE_URL:null PROTOCOL_VERSION:1.0 MACHINE_TYPE:teacup EXTRUDER_COUNT:1 REPETIER_PROTOCOL:\n"));
//*/
                break;

			case 116:
				//? --- M116: Wait ---
				//?
				//? Example: M116
				//?
				//? Wait for temperatures and other slowly-changing variables to arrive at their set values.
        temp_set_wait();
				break;

      case 119:
        //? --- M119: report endstop status ---
        //? Report the current status of the endstops configured in the
        //? firmware to the host.
        power_on();
        endstops_on();
        delay_ms(10); // allow the signal to stabilize
        {
          #if ! (defined(X_MIN_PIN) || defined(X_MAX_PIN) || \
                 defined(Y_MIN_PIN) || defined(Y_MAX_PIN) || \
                 defined(Z_MIN_PIN) || defined(Z_MAX_PIN))
            serial_writestr_P(PSTR("No endstops defined."));
          #else
            const char* const open = PSTR("open ");
            const char* const triggered = PSTR("triggered ");
          #endif

          #if defined(X_MIN_PIN)
            serial_writestr_P(PSTR("x_min:"));
            x_min() ? serial_writestr_P(triggered) : serial_writestr_P(open);
          #endif
          #if defined(X_MAX_PIN)
            serial_writestr_P(PSTR("x_max:"));
            x_max() ? serial_writestr_P(triggered) : serial_writestr_P(open);
          #endif
          #if defined(Y_MIN_PIN)
            serial_writestr_P(PSTR("y_min:"));
            y_min() ? serial_writestr_P(triggered) : serial_writestr_P(open);
          #endif
          #if defined(Y_MAX_PIN)
            serial_writestr_P(PSTR("y_max:"));
            y_max() ? serial_writestr_P(triggered) : serial_writestr_P(open);
          #endif
          #if defined(Z_MIN_PIN)
            serial_writestr_P(PSTR("z_min:"));
            z_min() ? serial_writestr_P(triggered) : serial_writestr_P(open);
          #endif
          #if defined(Z_MAX_PIN)
            serial_writestr_P(PSTR("z_max:"));
            z_max() ? serial_writestr_P(triggered) : serial_writestr_P(open);
          #endif
        }
        endstops_off();
        serial_writechar('\n');
        break;

      #ifdef EECONFIG
			case 130:
				//? --- M130: heater P factor ---
				//? Undocumented.
			  	//  P factor in counts per degreeC of error
        if ( ! next_target.seen_P)
          #ifdef HEATER_EXTRUDER
            next_target.P = HEATER_EXTRUDER;
          #else
            next_target.P = 0;
          #endif
				if (next_target.seen_S)
					pid_set_p(next_target.P, next_target.S);
				break;

			case 131:
				//? --- M131: heater I factor ---
				//? Undocumented.
			  	// I factor in counts per C*s of integrated error
        if ( ! next_target.seen_P)
          #ifdef HEATER_EXTRUDER
            next_target.P = HEATER_EXTRUDER;
          #else
            next_target.P = 0;
          #endif
				if (next_target.seen_S)
					pid_set_i(next_target.P, next_target.S);
				break;

			case 132:
				//? --- M132: heater D factor ---
				//? Undocumented.
			  	// D factor in counts per degreesC/second
        if ( ! next_target.seen_P)
          #ifdef HEATER_EXTRUDER
            next_target.P = HEATER_EXTRUDER;
          #else
            next_target.P = 0;
          #endif
				if (next_target.seen_S)
					pid_set_d(next_target.P, next_target.S);
				break;

			case 133:
				//? --- M133: heater I limit ---
				//? Undocumented.
        if ( ! next_target.seen_P)
          #ifdef HEATER_EXTRUDER
            next_target.P = HEATER_EXTRUDER;
          #else
            next_target.P = 0;
          #endif
				if (next_target.seen_S)
					pid_set_i_limit(next_target.P, next_target.S);
				break;

			case 134:
				//? --- M134: save PID settings to eeprom ---
				//? Undocumented.
				heater_save_settings();
				break;
            case 502:
                
                 reset_eeprom();
             case 205:
                sersendf_P(PSTR("EPR:3 153 %lq Zmax\n"),eeprom_read_dword((uint32_t *) &EE_real_zmax));
                sersendf_P(PSTR("EPR:3 1048 %d ADJTmp\n"),eeprom_read_dword((uint32_t *) &EE_adjust_temp));
                sersendf_P(PSTR("EPR:3 3 %lq StepX\n"),eeprom_read_dword((uint32_t *) &EE_stepx));
                sersendf_P(PSTR("EPR:3 7 %lq StepY\n"),eeprom_read_dword((uint32_t *) &EE_stepy));
                sersendf_P(PSTR("EPR:3 11 %lq StepZ\n"),eeprom_read_dword((uint32_t *) &EE_stepz));
                sersendf_P(PSTR("EPR:3 0 %lq StepE\n"),eeprom_read_dword((uint32_t *) &EE_stepe));

                sersendf_P(PSTR("EPR:2 15 %lu MFX\n"),eeprom_read_dword((uint32_t *) &EE_mfx));
                sersendf_P(PSTR("EPR:2 19 %lu MFY\n"),eeprom_read_dword((uint32_t *) &EE_mfy));
                sersendf_P(PSTR("EPR:2 23 %lu MFZ\n"),eeprom_read_dword((uint32_t *) &EE_mfz));
                sersendf_P(PSTR("EPR:2 27 %lu MFE\n"),eeprom_read_dword((uint32_t *) &EE_mfe));
                
                
                sersendf_P(PSTR("EPR:2 39 %lu XYJerk\n"),eeprom_read_dword((uint32_t *) &EE_jerkx));
                sersendf_P(PSTR("EPR:2 47 %lu Zjerk\n"),eeprom_read_dword((uint32_t *) &EE_jerkz));
                sersendf_P(PSTR("EPR:3 51 %lq Accel\n"),eeprom_read_dword((uint32_t *) &EE_accel));
                sersendf_P(PSTR("EPR:3 55 %lq Retract\n"),eeprom_read_dword((uint32_t *) &EE_retract));
                sersendf_P(PSTR("EPR:3 59 %lu Retract F\n"),eeprom_read_dword((uint32_t *) &EE_retractF));
                sersendf_P(PSTR("EPR:3 63 %lq Retract Z\n"),eeprom_read_dword((uint32_t *) &EE_retractZ));

                #ifdef DELTA_PRINTER
                sersendf_P(PSTR("EPR:3 133 %lq OfX\n"),eeprom_read_dword((uint32_t *) &EE_x_endstop_adj));
                sersendf_P(PSTR("EPR:3 137 %lq OfY\n"),eeprom_read_dword((uint32_t *) &EE_y_endstop_adj));
                sersendf_P(PSTR("EPR:3 141 %lq OfZ\n"),eeprom_read_dword((uint32_t *) &EE_z_endstop_adj));
                sersendf_P(PSTR("EPR:3 881 %lq RodLen\n"),eeprom_read_dword((uint32_t *) &EE_delta_diagonal_rod));
                sersendf_P(PSTR("EPR:3 885 %lq HorRad\n"),eeprom_read_dword((uint32_t *) &EE_delta_radius));
                sersendf_P(PSTR("EPR:3 889 %lq Segment\n"),eeprom_read_dword((uint32_t *) &EE_deltasegment));
                #endif
                
                recalc_acceleration(1);
                break;
             case 206:
             if (next_target.seen_X)next_target.S=next_target.target.axis[X];
             if (next_target.seen_P)
                switch (next_target.P) {
                    case 153:
                        if (next_target.seen_Y) {
                                home_set_zmax(next_target.target.axis[Y],1);
                        } else {
                                home_set_zmax(next_target.S,0);
                        }
                    break;
                    case 1048:
                        eeprom_write_dword((uint32_t *) &EE_adjust_temp,next_target.S/1000);
                        break;
                    case 0:
                        eeprom_write_dword((uint32_t *) &EE_stepe,next_target.S);
                        break;
                    case 3:
                        eeprom_write_dword((uint32_t *) &EE_stepx,next_target.S);
                        break;
                    case 7:
                        eeprom_write_dword((uint32_t *) &EE_stepy,next_target.S);
                        break;
                    case 11:
                        eeprom_write_dword((uint32_t *) &EE_stepz,next_target.S);
                        break;
                    case 15:
                        eeprom_write_dword((uint32_t *) &EE_mfx,next_target.S/1000);
                        break;
                    case 19:
                        eeprom_write_dword((uint32_t *) &EE_mfy,next_target.S/1000);
                        break;
                    case 23:
                        eeprom_write_dword((uint32_t *) &EE_mfz,next_target.S/1000);
                        break;
                    case 27:
                        eeprom_write_dword((uint32_t *) &EE_mfe,next_target.S/1000);
                        break;    
                    case 59:
                        eeprom_write_dword((uint32_t *) &EE_retractF,next_target.S/1000);
                        break;    
                    case 39:
                        eeprom_write_dword((uint32_t *) &EE_jerkx,next_target.S/1000);
                        break;
                    case 47:
                        eeprom_write_dword((uint32_t *) &EE_jerkz,next_target.S/1000);
                        break;
                    case 51:
                        eeprom_write_dword((uint32_t *) &EE_accel,next_target.S);
                        break;
                    case 55:
                        eeprom_write_dword((uint32_t *) &EE_retract,next_target.S);
                        break;  
                    case 63:
                        eeprom_write_dword((uint32_t *) &EE_retractZ,next_target.S);
                        break;  
                    #ifdef DELTA_PRINTER
                    case 133:
                        eeprom_write_dword((uint32_t *) &EE_x_endstop_adj,next_target.S);
                        break;
                    case 137:
                        eeprom_write_dword((uint32_t *) &EE_y_endstop_adj,next_target.S);
                        break;
                    case 141:
                        eeprom_write_dword((uint32_t *) &EE_z_endstop_adj,next_target.S);
                        break;
                    case 881:
                        eeprom_write_dword((uint32_t *) &EE_delta_diagonal_rod,next_target.S);
                        break;
                    case 885:
                        eeprom_write_dword((uint32_t *) &EE_delta_radius,next_target.S);
                        break;
                    case 889:
                        eeprom_write_dword((uint32_t *) &EE_deltasegment,next_target.S);
                        break;
                    #endif
                    }
                recalc_acceleration(1);
                
                break;
      #endif /* EECONFIG */

      #ifdef DEBUG
			case 136:
				//? --- M136: PRINT PID settings to host ---
				//? Undocumented.
				//? This comand is only available in DEBUG builds.
        if ( ! next_target.seen_P)
          #ifdef HEATER_EXTRUDER
            next_target.P = HEATER_EXTRUDER;
          #else
            next_target.P = 0;
          #endif
				heater_print(next_target.P);
				break;
      #endif /* DEBUG */

			case 140:
				//? --- M140: Set heated bed temperature ---
				//? Undocumented.
				#ifdef	HEATER_BED
					if ( ! next_target.seen_S)
						break;
					temp_set(HEATER_BED, next_target.S);
				#endif
				break;

      case 220:
        //? --- M220: Set speed factor override percentage ---
        if ( ! next_target.seen_S)
          break;
        // Scale 100% = 256
        next_target.target.f_multiplier = (next_target.S * 64 + 12) / 25;
        break;

      case 221:
        //? --- M221: Control the extruders flow ---
        if ( ! next_target.seen_S)
          break;
        // Scale 100% = 256
        next_target.target.e_multiplier = (next_target.S * 64 + 12) / 25;
        break;

      #ifdef DEBUG
			case 240:
				//? --- M240: echo off ---
				//? Disable echo.
				//? This command is only available in DEBUG builds.
				debug_flags &= ~DEBUG_ECHO;
				serial_writestr_P(PSTR("Echo off\n"));
				break;

			case 241:
				//? --- M241: echo on ---
				//? Enable echo.
				//? This command is only available in DEBUG builds.
				debug_flags |= DEBUG_ECHO;
				serial_writestr_P(PSTR("Echo on\n"));
				break;
      #endif /* DEBUG */

				// unknown mcode: spit an error
			default:
				sersendf_P(PSTR("E: Bad M-code %d\n"), next_target.M);
		} // switch (next_target.M)
	} // else if (next_target.seen_M)
} // process_gcode_command()
