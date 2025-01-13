#
# Copyright - See the COPYRIGHT that is included with this distribution.
# pvxs is distributed subject to a Software License Agreement found
# in file LICENSE that is included with this distribution.
#
# Author George S. McIntyre <george@level-n.com>, 2023
#

TOP=..

include $(TOP)/configure/CONFIG
# cfg/ sometimes isn't correctly included due to an issue in epics-base
# so we do here (maybe again) as workaround
-include $(wildcard $(TOP)/cfg/CONFIG*))
#----------------------------------------
#  ADD MACRO DEFINITIONS AFTER THIS LINE
#=============================

# access to private headers
USR_CPPFLAGS += -I$(TOP)/src
USR_CPPFLAGS += -DPVXS_IOC_API_BUILDING

DBD += pvxsIoc.dbd

INC += pvxs/iochooks.h

LIBRARY += pvxsIoc

SHRLIB_VERSION = $(PVXS_MAJOR_VERSION).$(PVXS_MINOR_VERSION)

pvxsIoc_SRCS += iochooks.cpp

ifdef BASE_3_15

pvxsIoc_SRCS += credentials.cpp
pvxsIoc_SRCS += channel.cpp
pvxsIoc_SRCS += demo.cpp
pvxsIoc_SRCS += dberrormessage.cpp
pvxsIoc_SRCS += imagedemo.c
pvxsIoc_SRCS += iocsource.cpp
pvxsIoc_SRCS += localfieldlog.cpp
pvxsIoc_SRCS += securityclient.cpp
pvxsIoc_SRCS += singlesource.cpp
pvxsIoc_SRCS += singlesourcehooks.cpp
pvxsIoc_SRCS += singlesrcsubscriptionctx.cpp
pvxsIoc_SRCS += typeutils.cpp

ifdef BASE_7_0

pvxsIoc_SRCS += field.cpp
pvxsIoc_SRCS += fielddefinition.cpp
pvxsIoc_SRCS += fieldname.cpp
pvxsIoc_SRCS += fieldsubscriptionctx.cpp
pvxsIoc_SRCS += group.cpp
pvxsIoc_SRCS += groupconfigprocessor.cpp
pvxsIoc_SRCS += groupprocessorcontext.cpp
pvxsIoc_SRCS += groupsource.cpp
pvxsIoc_SRCS += groupsourcehooks.cpp
pvxsIoc_SRCS += pvalink.cpp
pvxsIoc_SRCS += pvalink_channel.cpp
pvxsIoc_SRCS += pvalink_jlif.cpp
pvxsIoc_SRCS += pvalink_link.cpp
pvxsIoc_SRCS += pvalink_lset.cpp

else

pvxsIoc_SRCS += dummygroup.cpp

endif # BASE_7_0

pvxsIoc_LIBS += $(EPICS_BASE_IOC_LIBS)

else # BASE_3_15

pvxsIoc_SRCS += dummysingle.cpp
pvxsIoc_SRCS += dummygroup.cpp

pvxsIoc_LIBS += Com

endif # BASE_3_15

LIB_LIBS += pvxs
LIB_LIBS += $(EPICS_BASE_IOC_LIBS)
#===========================

include $(TOP)/configure/RULES
-include $(wildcard $(TOP)/cfg/RULES*))
#----------------------------------------
#  ADD RULES AFTER THIS LINE

ifdef BASE_7_0
../O.Common/pvxsIoc.dbd: ../pvxs7x.dbd
	$(CP) $< $@
else
../O.Common/pvxsIoc.dbd: ../pvxs3x.dbd
	$(CP) $< $@
endif
