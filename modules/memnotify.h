/**
 * @file memnotify.h
 * Memory use tracking and notification plugin for the Mode Control Entity
 * <p>
 * Copyright (C) 2014-2019 Jolla Ltd.
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

#ifndef MEMNOTIFY_H_
# define MEMNOTIFY_H_

/* ========================================================================= *
 * Settings
 * ========================================================================= */

/** Prefix for memnotify setting keys */
# define MCE_SETTING_MEMNOTIFY_PATH             "/system/osso/dsm/memnotify"

/** Memnotify warning level configuration */
# define MCE_SETTING_MEMNOTIFY_WARNING_PATH     MCE_SETTING_MEMNOTIFY_PATH"/warning"

/** Warning threshold for used memory [pages] */
# define MCE_SETTING_MEMNOTIFY_WARNING_USED     MCE_SETTING_MEMNOTIFY_PATH"/warning/used"
# define MCE_DEFAULT_MEMNOTIFY_WARNING_USED     0 // = disabled

/** Warning threshold for active memory [pages] */
# define MCE_SETTING_MEMNOTIFY_WARNING_ACTIVE   MCE_SETTING_MEMNOTIFY_PATH"/warning/active"
# define MCE_DEFAULT_MEMNOTIFY_WARNING_ACTIVE   0 // = disabled

/** Memnotify critical level configuration */
# define MCE_SETTING_MEMNOTIFY_CRITICAL_PATH    MCE_SETTING_MEMNOTIFY_PATH"/critical"

/** Critical threshold for used memory [pages] */
# define MCE_SETTING_MEMNOTIFY_CRITICAL_USED    MCE_SETTING_MEMNOTIFY_PATH"/critical/used"
# define MCE_DEFAULT_MEMNOTIFY_CRITICAL_USED    0 // = disabled

/** Critical threshold for active memory [pages] */
# define MCE_SETTING_MEMNOTIFY_CRITICAL_ACTIVE  MCE_SETTING_MEMNOTIFY_PATH"/critical/active"
# define MCE_DEFAULT_MEMNOTIFY_CRITICAL_ACTIVE  0 // = disabled

#endif /* MEMNOTIFY_H_ */
