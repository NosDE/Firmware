/****************************************************************************
 *
 *   Copyright (c) 2013-2015 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file mag_calibration.cpp
 *
 * Magnetometer calibration routine
 */

#include "mag_calibration.h"
#include "commander_helper.h"
#include "calibration_routines.h"
#include "calibration_messages.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <math.h>
#include <fcntl.h>
#include <drivers/drv_hrt.h>
#include <uORB/topics/sensor_combined.h>
#include <drivers/drv_mag.h>
#include <mavlink/mavlink_log.h>
#include <systemlib/param/param.h>
#include <systemlib/err.h>

/* oddly, ERROR is not defined for c++ */
#ifdef ERROR
# undef ERROR
#endif
static const int ERROR = -1;

static const char *sensor_name = "mag";

int calibrate_instance(int mavlink_fd, unsigned s, unsigned device_id);

int do_mag_calibration(int mavlink_fd)
{
	const unsigned max_mags = 3;

	int32_t device_id[max_mags];
	mavlink_and_console_log_info(mavlink_fd, CAL_STARTED_MSG, sensor_name);
	sleep(1);

	struct mag_scale mscale_null[max_mags] = {
	{
		0.0f,
		1.0f,
		0.0f,
		1.0f,
		0.0f,
		1.0f,
	}
	} ;

	int res = ERROR;

	char str[30];

	unsigned calibrated_ok = 0;

	for (unsigned s = 0; s < max_mags; s++) {

		/* erase old calibration */
		(void)sprintf(str, "%s%u", MAG_BASE_DEVICE_PATH, s);
		int fd = open(str, O_RDONLY);

		if (fd < 0) {
			continue;
		}

		mavlink_and_console_log_info(mavlink_fd, "Calibrating magnetometer #%u..", s);
		sleep(3);

		device_id[s] = ioctl(fd, DEVIOCGDEVICEID, 0);

		/* ensure all scale fields are initialized tha same as the first struct */
		(void)memcpy(&mscale_null[s], &mscale_null[0], sizeof(mscale_null[0]));

		res = ioctl(fd, MAGIOCSSCALE, (long unsigned int)&mscale_null[s]);

		if (res != OK) {
			mavlink_and_console_log_critical(mavlink_fd, CAL_FAILED_RESET_CAL_MSG);
		}

		if (res == OK) {
			/* calibrate range */
			res = ioctl(fd, MAGIOCCALIBRATE, fd);

			if (res != OK) {
				mavlink_and_console_log_info(mavlink_fd, "Skipped scale calibration");
				/* this is non-fatal - mark it accordingly */
				res = OK;
			}
		}

		close(fd);

		if (res == OK) {
			res = calibrate_instance(mavlink_fd, s, device_id[s]);

			if (res == OK) {
				calibrated_ok++;
			}
		}
	}

	if (calibrated_ok) {

		mavlink_and_console_log_info(mavlink_fd, CAL_PROGRESS_MSG, sensor_name, 100);
		usleep(100000);
		mavlink_and_console_log_info(mavlink_fd, CAL_DONE_MSG, sensor_name);

		/* auto-save to EEPROM */
		res = param_save_default();

		if (res != OK) {
			mavlink_and_console_log_critical(mavlink_fd, CAL_FAILED_SAVE_PARAMS_MSG);
		}
	} else {
		mavlink_and_console_log_critical(mavlink_fd, CAL_FAILED_MSG, sensor_name);
	}

	return res;
}

int calibrate_instance(int mavlink_fd, unsigned s, unsigned device_id)
{
	/* 45 seconds */
	uint64_t calibration_interval = 25 * 1000 * 1000;

	/* maximum 500 values */
	const unsigned int calibration_maxcount = 240;
	unsigned int calibration_counter;

	float *x = new float[calibration_maxcount];
	float *y = new float[calibration_maxcount];
	float *z = new float[calibration_maxcount];

	char str[30];
	int res = OK;
	
	/* allocate memory */
	mavlink_and_console_log_info(mavlink_fd, CAL_PROGRESS_MSG, sensor_name, 20);

	if (x == nullptr || y == nullptr || z == nullptr) {
		mavlink_and_console_log_critical(mavlink_fd, "ERROR: out of memory");

		/* clean up */
		if (x != nullptr) {
			delete x;
		}

		if (y != nullptr) {
			delete y;
		}

		if (z != nullptr) {
			delete z;
		}

		res = ERROR;
		return res;
	}

	if (res == OK) {
		int sub_mag = orb_subscribe_multi(ORB_ID(sensor_mag), s);

		if (sub_mag < 0) {
			mavlink_and_console_log_critical(mavlink_fd, "No mag found, abort");
			res = ERROR;
		}  else {
			struct mag_report mag;

			/* limit update rate to get equally spaced measurements over time (in ms) */
			orb_set_interval(sub_mag, (calibration_interval / 1000) / calibration_maxcount);

			/* calibrate offsets */
			uint64_t calibration_deadline = hrt_absolute_time() + calibration_interval;
			unsigned poll_errcount = 0;

			mavlink_and_console_log_info(mavlink_fd, "Turn on all sides: front/back,left/right,up/down");

			calibration_counter = 0U;

			while (hrt_absolute_time() < calibration_deadline &&
			       calibration_counter < calibration_maxcount) {

				/* wait blocking for new data */
				struct pollfd fds[1];
				fds[0].fd = sub_mag;
				fds[0].events = POLLIN;

				int poll_ret = poll(fds, 1, 1000);

				if (poll_ret > 0) {
					orb_copy(ORB_ID(sensor_mag), sub_mag, &mag);

					x[calibration_counter] = mag.x;
					y[calibration_counter] = mag.y;
					z[calibration_counter] = mag.z;

					calibration_counter++;

					if (calibration_counter % (calibration_maxcount / 20) == 0) {
						mavlink_and_console_log_info(mavlink_fd, CAL_PROGRESS_MSG, sensor_name, 20 + (calibration_counter * 50) / calibration_maxcount);
					}

				} else {
					poll_errcount++;
				}

				if (poll_errcount > 1000) {
					mavlink_and_console_log_critical(mavlink_fd, CAL_FAILED_SENSOR_MSG);
					res = ERROR;
					break;
				}
			}

			close(sub_mag);
		}
	}

	float sphere_x;
	float sphere_y;
	float sphere_z;
	float sphere_radius;

	if (res == OK && calibration_counter > (calibration_maxcount / 2)) {

		/* sphere fit */
		mavlink_and_console_log_info(mavlink_fd, CAL_PROGRESS_MSG, sensor_name, 70);
		sphere_fit_least_squares(x, y, z, calibration_counter, 100, 0.0f, &sphere_x, &sphere_y, &sphere_z, &sphere_radius);
		mavlink_and_console_log_info(mavlink_fd, CAL_PROGRESS_MSG, sensor_name, 80);

		if (!isfinite(sphere_x) || !isfinite(sphere_y) || !isfinite(sphere_z)) {
			mavlink_and_console_log_critical(mavlink_fd, "ERROR: NaN in sphere fit");
			res = ERROR;
		}
	}

	if (x != nullptr) {
		delete x;
	}

	if (y != nullptr) {
		delete y;
	}

	if (z != nullptr) {
		delete z;
	}

	if (res == OK) {
		/* apply calibration and set parameters */
		struct mag_scale mscale;
		(void)sprintf(str, "%s%u", MAG_BASE_DEVICE_PATH, s);
		int fd = open(str, 0);
		res = ioctl(fd, MAGIOCGSCALE, (long unsigned int)&mscale);

		if (res != OK) {
			mavlink_and_console_log_critical(mavlink_fd, "ERROR: failed to get current calibration");
		}

		if (res == OK) {
			mscale.x_offset = sphere_x;
			mscale.y_offset = sphere_y;
			mscale.z_offset = sphere_z;

			res = ioctl(fd, MAGIOCSSCALE, (long unsigned int)&mscale);

			if (res != OK) {
				mavlink_and_console_log_critical(mavlink_fd, CAL_FAILED_APPLY_CAL_MSG);
			}
		}

		close(fd);

		if (res == OK) {
			
			/* since temperature calibration is not yet in place, load matlab estimations */
			/* NOTE: hardcoded terms are only suitable for LSM303D (used in dataset) */
			if (device_id == 131594) {
				mscale.cal_temp   = 25.00f;
				mscale.min_temp   =  3.30f;
				mscale.max_temp   = 41.18f;

				/* terms are rounded to 15 digits */
				mscale.x3_temp[0] =  0.0000002037008925981353968f;
				mscale.x2_temp[0] = -0.0000053157482398091815412f;
				mscale.x1_temp[0] = -0.0008966009481810033321380f;

				mscale.x3_temp[1] = -0.0000000252839047476527412f;
				mscale.x2_temp[1] = -0.0000029153295599826378747f;
				mscale.x1_temp[1] =  0.0003352015919517725706100f;

				mscale.x3_temp[2] =  0.0000000083432984965270406f;
				mscale.x2_temp[2] =  0.0000064743926486698910593f;
				mscale.x1_temp[2] = -0.0014722041087225079536437f;
			}

			bool failed = false;
			/* set parameters */
			(void)sprintf(str, "CAL_MAG%u_ID", s);
			failed |= (OK != param_set(param_find(str), &(device_id)));
			(void)sprintf(str, "CAL_MAG%u_XOFF", s);
			failed |= (OK != param_set(param_find(str), &(mscale.x_offset)));
			(void)sprintf(str, "CAL_MAG%u_YOFF", s);
			failed |= (OK != param_set(param_find(str), &(mscale.y_offset)));
			(void)sprintf(str, "CAL_MAG%u_ZOFF", s);
			failed |= (OK != param_set(param_find(str), &(mscale.z_offset)));
			(void)sprintf(str, "CAL_MAG%u_XSCALE", s);
			failed |= (OK != param_set(param_find(str), &(mscale.x_scale)));
			(void)sprintf(str, "CAL_MAG%u_YSCALE", s);
			failed |= (OK != param_set(param_find(str), &(mscale.y_scale)));
			(void)sprintf(str, "CAL_MAG%u_ZSCALE", s);
			failed |= (OK != param_set(param_find(str), &(mscale.z_scale)));

			(void)sprintf(str, "CAL_MAG%u_TMPNOM", s);
			failed |= (OK != param_set(param_find(str), &(mscale.cal_temp)));
			(void)sprintf(str, "CAL_MAG%u_TMPMIN", s);
			failed |= (OK != param_set(param_find(str), &(mscale.min_temp)));
			(void)sprintf(str, "CAL_MAG%u_TMPMAX", s);
			failed |= (OK != param_set(param_find(str), &(mscale.max_temp)));
			for (unsigned j = 0; j < 3; j++) {
				(void)sprintf(str, "CAL_MAG%u_TA%uX0", s, j);
				failed |= (OK != param_set(param_find(str), &(mscale.x1_temp[j])));
				(void)sprintf(str, "CAL_MAG%u_TA%uX1", s, j);
				failed |= (OK != param_set(param_find(str), &(mscale.x2_temp[j])));
				(void)sprintf(str, "CAL_MAG%u_TA%uX2", s, j);
				failed |= (OK != param_set(param_find(str), &(mscale.x3_temp[j])));
			}
			
			if (failed) {
				res = ERROR;
				mavlink_and_console_log_critical(mavlink_fd, CAL_FAILED_SET_PARAMS_MSG);
			}

			mavlink_and_console_log_info(mavlink_fd, CAL_PROGRESS_MSG, sensor_name, 90);
		}

		mavlink_and_console_log_info(mavlink_fd, "mag off: x:%.2f y:%.2f z:%.2f Ga", (double)mscale.x_offset,
				 (double)mscale.y_offset, (double)mscale.z_offset);
		mavlink_and_console_log_info(mavlink_fd, "mag scale: x:%.2f y:%.2f z:%.2f", (double)mscale.x_scale,
				 (double)mscale.y_scale, (double)mscale.z_scale);
	}

	return res;
}
