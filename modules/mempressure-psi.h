/**
 * @file mempressure-psi.h
 * Memory use tracking and notification plugin for the Mode Control Entity
 * <p>
 * Copyright (c) 2014 - 2019 Jolla Ltd.
 * <p>
 * @author Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * @author Lukáš Karas <lukas.karas@avast.com>
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

#ifndef MEMPRESSURE_PSI_H_
# define MEMPRESSURE_PSI_H_

/* ========================================================================= *
 * Settings
 * ========================================================================= */

/** Prefix for mempressure setting keys */
# define MCE_SETTING_MEMPRESSURE_PSI_PATH              "/system/osso/dsm/mempressure-psi"

/** PSI tracking window [us] */
# define MCE_SETTING_MEMPRESSURE_PSI_WINDOW            MCE_SETTING_MEMPRESSURE_PSI_PATH"/window"
# define MCE_DEFAULT_MEMPRESSURE_PSI_WINDOW            1000000

/** Memnotify warning level configuration */
# define MCE_SETTING_MEMPRESSURE_PSI_WARNING_PATH      MCE_SETTING_MEMPRESSURE_PSI_PATH"/warning"

/** Warning threshold stall stall time [us] */
# define MCE_SETTING_MEMPRESSURE_PSI_WARNING_STALL     MCE_SETTING_MEMPRESSURE_PSI_PATH"/warning/stall"
# define MCE_DEFAULT_MEMPRESSURE_PSI_WARNING_STALL     100000

/** Warning threshold type (some or full) */
# define MCE_SETTING_MEMPRESSURE_PSI_WARNING_TYPE      MCE_SETTING_MEMPRESSURE_PSI_PATH"/warning/type"
# define MCE_DEFAULT_MEMPRESSURE_PSI_WARNING_TYPE      "some"

/** Memnotify critical level configuration */
# define MCE_SETTING_MEMPRESSURE_PSI_CRITICAL_PATH     MCE_SETTING_MEMPRESSURE_PSI_PATH"/critical"

/** Critical threshold for stall time [us] */
# define MCE_SETTING_MEMPRESSURE_PSI_CRITICAL_STALL    MCE_SETTING_MEMPRESSURE_PSI_PATH"/critical/stall"
# define MCE_DEFAULT_MEMPRESSURE_PSI_CRITICAL_STALL    150000

/** Critica threshold type (some or full) */
# define MCE_SETTING_MEMPRESSURE_PSI_CRITICAL_TYPE     MCE_SETTING_MEMPRESSURE_PSI_PATH"/critical/type"
# define MCE_DEFAULT_MEMPRESSURE_PSI_CRITICAL_TYPE     "full"

#endif /* MEMPRESSURE_PSI_H_ */
