/**
 * @file filter-brightness-als.h
 * Headers for the Ambient Light Sensor level adjusting filter module
 * for display backlight, key backlight, and LED brightness
 * <p>
 * Copyright © 2007-2011 Nokia Corporation and/or its subsidiary(-ies).
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Tuomo Tanskanen <ext-tuomo.1.tanskanen@nokia.com>
 *
 * mce is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * mce is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mce.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _FILTER_BRIGHTNESS_ALS_H_
# define _FILTER_BRIGHTNESS_ALS_H_

/** Name of common group in color profiles conf file */
# define MCE_CONF_COMMON_GROUP           "Common"

/** Name of default color profile id key in color profiles conf file */
# define MCE_CONF_DEFAULT_PROFILE_ID_KEY "DefaultProfile"

/** Name of the hardcoded color profile */
# define COLOR_PROFILE_ID_HARDCODED      "hardcoded"

/** Name to use for requesting (configured) default profile */
# define COLOR_PROFILE_ID_DEFAULT        "default"

#endif /* _FILTER_BRIGHTNESS_ALS_H_ */
