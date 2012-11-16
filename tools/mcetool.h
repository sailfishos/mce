/**
 * @file mcetool.h
 * Headers for the mcetool
 * <p>
 * Copyright © 2005-2008, 2011 Nokia Corporation and/or its subsidiary(-ies).
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
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
#ifndef _MCETOOL_H_
#define _MCETOOL_H_

#include <glib.h>
#include <locale.h>

#ifdef ENABLE_NLS
#include <libintl.h>
/** _() to use when NLS is enabled */
#define _(__str)		gettext(__str)
#else
#undef bindtextdomain
/** Dummy bindtextdomain to use when NLS is disabled */
#define bindtextdomain(__domain, __directory)
#undef textdomain
/** Dummy textdomain to use when NLS is disabled */
#define textdomain(__domain)
/** Dummy _() to use when NLS is disabled */
#define _(__str)		__str
#endif /* ENABLE_NLS */

#endif /* _MCETOOL_H_ */
