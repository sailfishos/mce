/**
 * @file inactivity.h
 * Inactivity module -- this implements inactivity logic for MCE
 * <p>
 * Copyright (C) 2018      Jolla Ltd.
 * <p>
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
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

#ifndef  MCE_MODULES_INACTIVITY_H_
# define MCE_MODULES_INACTIVITY_H_

/* ========================================================================= *
 * Settings for inactivity plugin
 * ========================================================================= */

/** Prefix used for inactivity plugin setting keys */
# define MCE_SETTING_INACTIVITY_PATH             "/system/osso/dsm/inactivity"

/** Delay for automatic shutdown due to inactivity */
# define MCE_SETTING_INACTIVITY_SHUTDOWN_DELAY   MCE_SETTING_INACTIVITY_PATH "/shutdown_delay"
# define MCE_DEFAULT_INACTIVITY_SHUTDOWN_DELAY   0
# define MCE_MINIMUM_INACTIVITY_SHUTDOWN_DELAY   30

#endif /* MCE_MODULES_INACTIVITY_H_ */
