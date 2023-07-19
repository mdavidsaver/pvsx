#!/usr/bin/env python

import os
import re

from glob import glob

from distutils import log
from distutils.errors import CompileError
from setuptools import Command, Distribution
from setuptools_dso import DSO, Extension, setup, build_dso, ProbeToolchain
from epicscorelibs.config import get_config_var
import epicscorelibs.path
import epicscorelibs.version

def pvxsversion():
    with open(os.path.join('configure', 'CONFIG_PVXS_VERSION'), 'r') as F:
        return {M.group(1):M.group(2) for M in re.finditer(r'([A-Z_]+)\s*=\s*(\d+)', F.read())}
pvxsversion = pvxsversion()

def eventversion():
    defs = {}
    with open(os.path.join('bundle', 'libevent', 'cmake','VersionViaGit.cmake'), 'r') as F:
        for M in re.finditer(r'set\s*\(\s*([A-Z_]+)\s+"?([^"\s]+)"?\s*\)', F.read()):
            # there will be multiple definitions, use the first
            defs.setdefault(M.group(1), M.group(2))

    A = defs['EVENT_GIT___VERSION_MAJOR']
    B = defs['EVENT_GIT___VERSION_MINOR']
    C = defs['EVENT_GIT___VERSION_PATCH']
    D = 1 if defs['EVENT_GIT___VERSION_STAGE'].endswith('dev') else 0
    ver = '.'.join((A,B,C))
    full = '%s-%s'%(ver, defs['EVENT_GIT___VERSION_STAGE'])
    num = (int(A)<<24) | (int(B)<<16) | (int(C)<<8) | D

    return {
        'EVENT_VERSION_MAJOR':A,
        'EVENT_VERSION_MINOR':B,
        'EVENT_VERSION_PATCH':C,
        'EVENT_NUMERIC_VERSION':'0x%08x'%num,
        'EVENT_PACKAGE_VERSION':ver,
        'EVENT_VERSION':full,
    }
eventversion = eventversion()

def cexpand(iname, oname, defs={}, dry_run=False):
    """Expand input file to output file.
    
    defs dict used to expand "@MACRO@", or replace "#cmakedefine MACRO VALUE" lines
    """
    log.info('expand %s -> %s', iname, oname)
    log.debug('With %s', defs)
    with open(iname, 'r') as F:
        inp = F.read()

    def msub(M):
        ret= defs[M.group(1)]
        assert isinstance(ret, str), (M.group(1), ret)
        return ret
    out = re.sub(r'@([^@]+)@', msub, inp)

    def csub(M):
        act = defs[M.group(1)]
        if act is None:
            return '/* #undef %s */'%M.group(1)
        elif M.lastindex>1:
            return '#define %s %s'%(M.group(1), M.group(2))
        else:
            return '#define %s'%(M.group(1))
    out = re.sub(r'^\s*#\s*cmakedefine\s+([^\s]+)(?:\s+(.*))?$', csub, out, 0, re.MULTILINE)

    if dry_run:
        log.info('Would write %s', oname)
    else:
        log.info('Write %s', oname)
        log.info('>>>>>>>>')
        log.info('%s', out)
        log.info('<<<<<<<<')
        with open(oname, 'w') as F:
            F.write(out)

def logexc(fn):
    def wrapit(*args, **kws):
        try:
            return fn(*args, **kws)
        except:
            import traceback
            traceback.print_exc()
            raise
    return wrapit

class Expand(Command):
    user_options = [
        ('build-lib=', 't',
         "directory for temporary files (build by-products)"),
        ('build-temp=', 't',
         "directory for temporary files (build by-products)"),
    ]
    def initialize_options(self):
        self.build_lib = None
        self.build_temp = None

    def finalize_options(self):
        self.set_undefined_options('build',
                                   ('build_lib', 'build_lib'),
                                   ('build_temp', 'build_temp'),
                                  )

    @logexc
    def run(self):
        log.info("In Expand")
        self.mkpath(os.path.join(self.build_temp, 'event2'))
        self.mkpath(os.path.join(self.build_lib, 'pvxslibs', 'include', 'pvxs'))
        self.mkpath(os.path.join(self.build_lib, 'pvxslibs', 'dbd'))

        OS_CLASS = get_config_var('OS_CLASS')

        self.distribution.DEFS = DEFS = {
            # *NIX unsupported by EPICS Base
            '_ALL_SOURCE':None,
            '_TANDEM_SOURCE':None,
            '__EXTENSIONS__':None,
            '_MINIX':None,
            '__EXT_POSIX2':None,
            '_LARGE_FILES':None,
            '_POSIX_PTHREAD_SEMANTICS':None,
            'EVENT__HAVE_SA_FAMILY_T':None,
            # unused
            '_FILE_OFFSET_BITS':None,
            '_POSIX_1_SOURCE':None,
            '_POSIX_SOURCE':None,
            # config
            'EVENT__DISABLE_DEBUG_MODE':None,
            'EVENT__DISABLE_MM_REPLACEMENT':None,
            'EVENT__DISABLE_THREAD_SUPPORT':None,
            'EVENT__HAVE_LIBZ':None,
            'EVENT__HAVE_OPENSSL':None,
            'EVENT__HAVE_MBEDTLS':None,
        }

        DEFS.update(pvxsversion) # PVXS_*_VERSION
        DEFS.update(eventversion) # EVENT*VERSION

        for var in ('EPICS_HOST_ARCH', 'T_A', 'OS_CLASS', 'CMPLR_CLASS'):
            DEFS[var] = get_config_var(var)

        probe = ProbeToolchain()

        if probe.check_symbol('__GNU_LIBRARY__', headers=['features.h']):
            DEFS['_GNU_SOURCE'] = '1'
            probe.define_macros += [('_GNU_SOURCE', None)]
        else:
            DEFS['_GNU_SOURCE'] = None

        if OS_CLASS=='WIN32':
            probe.headers += ['winsock2.h', 'ws2tcpip.h']
            probe.define_macros += [('Iwinsock2.h', None), ('Iws2tcpip.h', None), ('_WIN32_WINNT', '0x0600')]

            if probe.check_include('afunix.h'):
                DEFS['EVENT__HAVE_AFUNIX_H'] = '1'
                probe.headers.append('afunix.h')
            else:
                DEFS['EVENT__HAVE_AFUNIX_H'] = None

            DEFS.update({
                'EVENT__HAVE_INET_NTOP':'0',
                'EVENT__HAVE_INET_PTON':'0',
                'EVENT__HAVE_PTHREADS':'0',
                'EVENT__HAVE_WEPOLL':'1',
            })

        else:
            DEFS.update({
                'EVENT__HAVE_AFUNIX_H':None,
                'EVENT__HAVE_PTHREADS':'1',
                'EVENT__HAVE_WEPOLL':None,
            })
            probe.headers += ['pthread.h']

        for hfile, isextra in [('sys/types.h', True),
                            ('sys/socket.h', True),
                            ('sys/random.h', True),
                            ('netinet/in.h', True),
                            ('sys/un.h', True),
                            ('netinet/in6.h', True),
                            ('unistd.h', False),
                            ('netdb.h', False),
                            ('dlfcn.h', False),
                            ('arpa/inet.h', False),
                            ('fcntl.h', True),
                            ('inttypes.h', False),
                            ('memory.h', False),
                            ('poll.h', False),
                            ('port.h', True),
                            ('signal.h', False),
                            ('stdarg.h', False),
                            ('stddef.h', False),
                            ('stdint.h', False),
                            ('stdlib.h', False),
                            ('strings.h', False),
                            ('string.h', False),
                            ('sys/devpoll.h', False),
                            ('sys/epoll.h', False),
                            ('sys/eventfd.h', False),
                            ('sys/event.h', False),
                            ('sys/ioctl.h', False),
                            ('sys/mman.h', False),
                            ('sys/param.h', False),
                            ('sys/queue.h', False),
                            ('sys/select.h', False),
                            ('sys/sendfile.h', False),
                            ('sys/stat.h', False),
                            ('sys/time.h', True),
                            ('sys/uio.h', False),
                            ('sys/types.h', True),
                            ('ifaddrs.h', False),
                            ('mach/mach_time.h', False),
                            ('mach/mach.h', False),
                            ('netinet/tcp.h', False),
                            ('sys/wait.h', False),
                            ('sys/resource.h', False),
                            ('sys/sysctl.h', False), # TODO !linux
                            ('sys/timerfd.h', False),
                            ('sys/signalfd.h', False),
                            ('errno.h', False)]:
            if probe.check_include(hfile):
                DEFS['EVENT__HAVE_'+hfile.upper().replace('/','_').replace('.','_')] = '1'
                probe.headers.append(hfile)
            else:
                DEFS['EVENT__HAVE_'+hfile.upper().replace('/','_').replace('.','_')] = None

        for sym in ['epoll_create',
                'epoll_ctl',
                'eventfd',
                'clock_gettime',
                'fcntl',
                'gettimeofday',
                'kqueue',
                'mmap',
                'mmap64',
                'pipe',
                'pipe2',
                'pread',
                'poll',
                'port_create',
                'sendfile',
                'sigaction',
                'signal',
                'strsignal',
                'splice',
                'strlcpy',
                'strsep',
                'strtok_r',
                'vasprintf',
                'sysctl',
                'accept4',
                'arc4random',
                'arc4random_buf',
                'arc4random_addrandom',
                'epoll_create1',
                'epoll_pwait2',
                'getegid',
                'geteuid',
                'getifaddrs',
                'issetugid',
                'mach_absolute_time',
                'nanosleep',
                'usleep',
                'timeradd',
                'timerfd_create',
                'pthread_mutexattr_setprotocol',
                'setenv',
                'setrlimit',
                'umask',
                'unsetenv',
                'gethostbyname_r',
                'getservbyname',
                'select',
                '_gmtime64_s',
                '_gmtime64',
                '__FUNCTION__',
                'F_SETFD',
                'getrandom',
                'getaddrinfo',
                'getnameinfo',
                'getprotobynumber',
                'inet_ntop',
                'inet_pton',
                'strtoll',
                'timerclear',
                'timercmp',
                'timerisset',
                'putenv']:
            DEFS['EVENT__HAVE_'+sym.upper().replace('/','_').replace('.','_')] = '1' if probe.check_symbol(sym) else None

        DEFS['EVENT__HAVE___func__'] = '1' if probe.check_symbol('__func__') else None

        DEFS['EVENT__HAVE_EPOLL'] = DEFS['EVENT__HAVE_EPOLL_CREATE']
        DEFS['EVENT__HAVE_SIGNALFD'] = DEFS['EVENT__HAVE_SYS_SIGNALFD_H']
        DEFS['EVENT__HAVE_DEVPOLL'] = DEFS['EVENT__HAVE_SYS_DEVPOLL_H']

        DEFS['EVENT__HAVE_TAILQFOREACH'] = '1' if probe.check_symbol('TAILQ_FOREACH', ['sys/queue.h']) else None
        DEFS['EVENT__HAVE_DECL_CTL_KERN'] = '1' if probe.check_symbol('CTL_KERN', ['sys/sysctl.h']) else '0'
        DEFS['EVENT__HAVE_DECL_KERN_ARND'] = '1' if probe.check_symbol('KERN_ARND', ['sys/sysctl.h']) else '0'
        DEFS['EVENT__HAVE_FD_MASK'] = '1' if probe.check_symbol('FD_MASK') else '0'
        DEFS['EVENT__HAVE_SETFD'] = '1' if probe.check_symbol('F_SETFD') else '0'

        DEFS['EVENT__DNS_USE_CPU_CLOCK_FOR_ID'] = DEFS['EVENT__HAVE_CLOCK_GETTIME']

        DEFS['EVENT__DNS_USE_GETTIMEOFDAY_FOR_ID'] = None
        DEFS['EVENT__DNS_USE_FTIME_FOR_ID'] = None

        DEFS['EVENT__HAVE_EVENT_PORTS'] = '1' if DEFS['EVENT__HAVE_PORT_H']=='1' and DEFS['EVENT__HAVE_PORT_CREATE']=='1' else None

        if OS_CLASS=='WIN32':
            # windows has select(), but support comes from win32select.c instead of the usual select.c
            DEFS['EVENT__HAVE_SELECT'] = None

        for kw in ['inline', 'size_t']:
            DEFS['EVENT__'+kw] = kw
        DEFS['EVENT__inline'] = 'inline'
        DEFS['EVENT__size_t'] = 'size_t'

        DEFS['EVENT__HAVE_GETHOSTBYNAME_R_3_ARG'] = '1' if probe.try_compile('''
            #include <netdb.h>
            int wrap_gethostbyname_r(const char *name, struct hostent *hp, struct hostent_data *hdata) {
                return gethostbyname_r(name, hp, hdata);
            }
            ''') else None

        DEFS['EVENT__HAVE_GETHOSTBYNAME_R_5_ARG'] = '1' if probe.try_compile('''
            #include <netdb.h>
            struct hostent *wrap_gethostbyname_r(const char *name, struct hostent *hp, char *buf, size_t buflen, int *herr) {
                return gethostbyname_r(name, hp, buf, buflen, herr);
            }
            ''') else None

        DEFS['EVENT__HAVE_GETHOSTBYNAME_R_6_ARG'] = '1' if probe.try_compile('''
            #include <netdb.h>
            int wrap_gethostbyname_r(const char *name, struct hostent *hp, char *buf, size_t buflen, struct hostent **result, int *herr) {
                return gethostbyname_r(name, hp, buf, buflen, result, herr);
            }
            ''') else None

        for type in ['uint8_t',
                    'uint16_t',
                    'uint32_t',
                    'uint64_t',
                    'short',
                    'int',
                    'unsigned',
                    'unsigned',
                    'unsigned int',
                    'long',
                    'long long',
                    'void *',
                    'pthread_t',
                    'uintptr_t',
                    'size_t',
                    #'ssize_t',
                    #'SSIZE_T',
                    'off_t',
                    'socklen_t',
                    'pid_t',
                    'sa_family_t',
                    'struct addrinfo',
                    'struct in6_addr',
                    'struct sockaddr_in',
                    'struct sockaddr_in6',
                    'struct sockaddr_un',
                    'struct sockaddr_storage',
                    'time_t',
                    'struct linger']:
            mangled_type = type.upper().replace(' ','_').replace('*', 'P')
            try:
                DEFS['EVENT__SIZEOF_'+mangled_type] = str(probe.sizeof(type))
            except CompileError:
                DEFS['EVENT__SIZEOF_'+mangled_type] = '0'
                DEFS['EVENT__HAVE_'+mangled_type] = None
            else:
                DEFS['EVENT__HAVE_'+mangled_type] = '1'

        try:
            DEFS['EVENT__SIZEOF_SSIZE_T'] = str(probe.sizeof('ssize_t'))
            DEFS['EVENT__ssize_t'] = 'ssize_t'
        except CompileError:
            DEFS['EVENT__SIZEOF_SSIZE_T'] = str(probe.sizeof('SSIZE_T'))
            DEFS['EVENT__ssize_t'] = 'SSIZE_T'
            # libevent CMakeLists.txt defaults to 'int' if neither is available, which seems wrong...
        DEFS['EVENT__HAVE_SSIZE_T'] = '1'

        if DEFS['EVENT__SIZEOF_SOCKLEN_T']!='0':
            DEFS['EVENT__socklen_t'] = 'socklen_t'

        else:
            DEFS['EVENT__socklen_t'] = 'unsigned int'
            DEFS['EVENT__SIZEOF_SOCKLEN_T'] = DEFS['EVENT__SIZEOF_UNSIGNED_INT']

        for struct, member in [('struct in6_addr', 's6_addr16'),
                               ('struct in6_addr', 's6_addr32'),
                               ('struct sockaddr_in6', 'sin6_len'),
                               ('struct sockaddr_in', 'sin_len'),
                               ('struct sockaddr_storage', 'ss_family'),
                               ('struct sockaddr_storage', '__ss_family')]:
            mangled_mem = ('EVENT__HAVE_%s_%s'%(struct, member)).upper().replace(' ','_')
            DEFS[mangled_mem] = '1' if probe.check_member(struct, member) else None

        DEFS['EVENT__TIME_WITH_SYS_TIME'] = None

        # TODO: assume working kqueue
        DEFS['EVENT__HAVE_WORKING_KQUEUE'] = DEFS['EVENT__HAVE_KQUEUE']
        print(DEFS)


        cexpand('src/describe.h@',
                os.path.join(self.build_temp, 'describe.h'),
                DEFS, dry_run=self.dry_run)
        cexpand('src/pvxs/versionNum.h@',
                os.path.join(self.build_lib, 'pvxslibs', 'include', 'pvxs', 'versionNum.h'),
                DEFS, dry_run=self.dry_run)
        cexpand('bundle/libevent/evconfig-private.h.cmake',
                os.path.join(self.build_temp, 'evconfig-private.h'),
                DEFS, dry_run=self.dry_run)
        cexpand('bundle/libevent/event-config.h.cmake',
                os.path.join(self.build_temp, 'event2', 'event-config.h'),
                DEFS, dry_run=self.dry_run)

        if self.dry_run:
            log.info('Would create pvxsVCS.h')
        else:
            log.info('Writing pvxsVCS.h')
            with open(os.path.join(self.build_temp, 'pvxsVCS.h'), 'w') as F:
                F.write('''
#ifndef PVXS_VCS_VERSION
#  define PVXS_VCS_VERSION "pip"
#endif 
''')

class InstallHeaders(Command):
    user_options = [
        ('build-lib=', 't',
         "directory for temporary files (build by-products)"),
        ('build-temp=', 't',
         "directory for temporary files (build by-products)"),
    ]
    def initialize_options(self):
        self.build_lib = None
        self.build_temp = None

    def finalize_options(self):
        self.set_undefined_options('build',
                                   ('build_lib', 'build_lib'),
                                   ('build_temp', 'build_temp'),
                                  )
    def run(self):
        log.info("In InstallHeaders")

        for header in glob('src/pvxs/*.h'):
            self.copy_file(header,
                           os.path.join(self.build_lib, 'pvxslibs', 'include', os.path.relpath(header, 'src')))

        for header in glob('ioc/pvxs/*.h'):
            self.copy_file(header,
                           os.path.join(self.build_lib, 'pvxslibs', 'include', os.path.relpath(header, 'ioc')))

        self.copy_file('ioc/pvxs7x.dbd',
                       os.path.join(self.build_lib, 'pvxslibs/dbd/pvxsIoc.dbd'))

@logexc
def define_DSOS(self):
    DEFS = self.distribution.DEFS
    OS_CLASS = get_config_var('OS_CLASS')

    src_core = [
        'buffer.c',
        'bufferevent.c',
        'bufferevent_filter.c',
        'bufferevent_pair.c',
        'bufferevent_ratelim.c',
        'bufferevent_sock.c',
        'event.c',
        'evmap.c',
        'evthread.c',
        'evutil.c',
        'evutil_rand.c',
        'evutil_time.c',
        'watch.c',
        'listener.c',
        'log.c',
        'signal.c',
        'strlcpy.c',
    ]

    if DEFS['EVENT__HAVE_POLL']=='1':
        src_core += ['poll.c']

    if DEFS['EVENT__HAVE_KQUEUE']=='1':
        src_core += ['kqueue.c']

    if DEFS['EVENT__HAVE_DEVPOLL']=='1':
        src_core += ['devpoll.c']

    if DEFS['EVENT__HAVE_WEPOLL']=='1':
        src_core += ['wepoll.c', 'epoll.c']
    elif DEFS['EVENT__HAVE_EPOLL']=='1':
        src_core += ['epoll.c']

    if DEFS['EVENT__HAVE_EVENT_PORTS']=='1':
        src_core += ['evport.c']

    if DEFS['EVENT__HAVE_SIGNALFD']=='1':
        src_core += ['signalfd.c']

    if OS_CLASS=='WIN32':
        src_core += [
            'buffer_iocp.c',
            'bufferevent_async.c',
            'event_iocp.c',
            'win32select.c',
            'evthread_win32.c',
        ]
    elif DEFS['EVENT__HAVE_SELECT']=='1':
        src_core += ['select.c']

    src_core = [os.path.join('bundle', 'libevent', src) for src in src_core]

    src_pvxs = [
        'describe.cpp',
        'log.cpp',
        'unittest.cpp',
        'util.cpp',
        'osgroups.cpp',
        'sharedarray.cpp',
        'bitmask.cpp',
        'type.cpp',
        'data.cpp',
        'datafmt.cpp',
        'pvrequest.cpp',
        'dataencode.cpp',
        'nt.cpp',
        'evhelper.cpp',
        'udp_collector.cpp',
        'config.cpp',
        'conn.cpp',
        'server.cpp',
        'serverconn.cpp',
        'serverchan.cpp',
        'serverintrospect.cpp',
        'serverget.cpp',
        'servermon.cpp',
        'serversource.cpp',
        'sharedpv.cpp',
        'client.cpp',
        'clientreq.cpp',
        'clientconn.cpp',
        'clientintrospect.cpp',
        'clientget.cpp',
        'clientmon.cpp',
        'clientdiscover.cpp',
    ]

    src_pvxs = [os.path.join('src', src) for src in src_pvxs]

    if OS_CLASS=='WIN32':
        src_pvxs += ['src/os/WIN32/osdSockExt.cpp']
    else:
        src_pvxs += ['src/os/default/osdSockExt.cpp']

    event_libs = []
    if OS_CLASS=='WIN32':
        event_libs = ['ws2_32','shell32','advapi32','bcrypt','iphlpapi']

    src_pvxsIoc = [
        "ioc/channel.cpp",
        "ioc/credentials.cpp",
        "ioc/dberrormessage.cpp",
        "ioc/demo.cpp",
        "ioc/field.cpp",
        "ioc/fielddefinition.cpp",
        "ioc/fieldname.cpp",
        "ioc/fieldsubscriptionctx.cpp",
        "ioc/groupconfigprocessor.cpp",
        "ioc/group.cpp",
        "ioc/groupprocessorcontext.cpp",
        "ioc/groupsource.cpp",
        "ioc/groupsourcehooks.cpp",
        "ioc/imagedemo.c",
        "ioc/iochooks.cpp",
        "ioc/iocsource.cpp",
        "ioc/localfieldlog.cpp",
        "ioc/securityclient.cpp",
        "ioc/singlesource.cpp",
        "ioc/singlesourcehooks.cpp",
        "ioc/singlesrcsubscriptionctx.cpp",
        "ioc/typeutils.cpp",
        "ioc/pvalink_channel.cpp",
        "ioc/pvalink.cpp",
        "ioc/pvalink_jlif.cpp",
        "ioc/pvalink_link.cpp",
        "ioc/pvalink_lset.cpp",
    ]

    probe = ProbeToolchain()

    cxx11_flags = []
    if probe.try_compile('int probefn() { auto x=1; return x; }',
                         language='c++',
                         extra_preargs=['-std=c++11']):
        cxx11_flags += ['-std=c++11']

    pvxs_abi = '%(PVXS_MAJOR_VERSION)s.%(PVXS_MINOR_VERSION)s'%pvxsversion
    event_abi = '%(EVENT_VERSION_MAJOR)s.%(EVENT_VERSION_MINOR)s.%(EVENT_VERSION_PATCH)s'%eventversion

    DSOS = []

    dsos_pvxs = [
        'epicscorelibs.lib.Com',
        'pvxslibs.lib.event_core',
    ]

    if OS_CLASS!='WIN32':
        DSOS += [DSO('pvxslibs.lib.event_pthread',
                [os.path.join('bundle', 'libevent', 'evthread_pthread.c')],
                define_macros = [('event_pthreads_shared_EXPORTS', None)],
                include_dirs=[
                    'bundle/libevent/include',
                    'bundle/libevent/compat', # for sys/queue.h
                    '.'
                ],
                soversion = event_abi,
        )]
        dsos_pvxs += ['pvxslibs.lib.event_pthread']

    DSOS += [
        DSO('pvxslibs.lib.event_core', src_core,
            define_macros = [('event_core_shared_EXPORTS', None)],
            include_dirs=[
                'bundle/libevent/include',
                'bundle/libevent/compat', # for sys/queue.h
                '.'
            ],
            soversion = event_abi,
            libraries = event_libs,
        ),
        DSO('pvxslibs.lib.pvxs', src_pvxs,
            define_macros = [('PVXS_API_BUILDING', None), ('PVXS_ENABLE_EXPERT_API', None)] + get_config_var('CPPFLAGS'),
            include_dirs=[
                'bundle/libevent/include',
                'src',
                '.', # generated headers under build/tmp
                'pvxslibs/include', # generated headers under build/lib
                epicscorelibs.path.include_path
                ],
            lang_compile_args = {
                'c': get_config_var('CFLAGS'),
                'c++': cxx11_flags + get_config_var('CXXFLAGS'),
            },
            extra_link_args = cxx11_flags + get_config_var('LDFLAGS'),
            soversion = pvxs_abi,
            dsos = dsos_pvxs,
            libraries = get_config_var('LDADD') + event_libs,
        ),
        DSO('pvxslibs.lib.pvxsIoc', src_pvxsIoc,
            define_macros = [('PVXS_IOC_API_BUILDING', None), ('PVXS_ENABLE_EXPERT_API', None)] + get_config_var('CPPFLAGS'),
            include_dirs=[
                'bundle/libevent/include',
                'src',
                'ioc',
                '.', # generated headers under build/tmp
                'pvxslibs/include', # generated headers under build/lib
                epicscorelibs.path.include_path
                ],
            lang_compile_args = {
                'c': get_config_var('CFLAGS'),
                'c++': cxx11_flags + get_config_var('CXXFLAGS'),
            },
            extra_link_args = cxx11_flags + get_config_var('LDFLAGS'),
            soversion = pvxs_abi,
            dsos = [
                'pvxslibs.lib.pvxs',
                'pvxslibs.lib.event_core',
                'epicscorelibs.lib.dbRecStd',
                'epicscorelibs.lib.dbCore',
                #'epicscorelibs.lib.ca',
                'epicscorelibs.lib.Com',
            ],
            libraries = get_config_var('LDADD') + event_libs,
        )
    ]

    return DSOS

build_dso.sub_commands.extend([
    ('build_expand', lambda self:True),
    ('install_epics_headers', lambda self:True),
])


pvxs_ver = '%(PVXS_MAJOR_VERSION)s.%(PVXS_MINOR_VERSION)s.%(PVXS_MAINTENANCE_VERSION)s'%pvxsversion
#pvxs_ver += 'a2'

with open(os.path.join(os.path.dirname(__file__), 'README.md')) as F:
    long_description = F.read()

setup(
    name='pvxslibs',
    version=pvxs_ver,
    description="PVXS libraries packaged for python",
    long_description=long_description,
    long_description_content_type='text/markdown',
    url='https://epics-base.github.io/pvxs',
    author='Michael Davidsaver',
    author_email='mdavidsaver@gmail.com',
    license='BSD',
    classifiers = [
        'Development Status :: 5 - Production/Stable',
        'Programming Language :: Python :: 2.7',
        'Programming Language :: Python :: 3',
        'Programming Language :: Python :: Implementation :: CPython',
        'License :: OSI Approved :: BSD License',
        'Intended Audience :: Science/Research',
        'Topic :: Scientific/Engineering',
        'Topic :: Software Development :: Libraries',
        'Topic :: System :: Distributed Computing',
        'Operating System :: POSIX :: Linux',
        'Operating System :: MacOS',
        'Operating System :: Microsoft :: Windows',
    ],
    keywords='epics scada',
    python_requires='>=2.7',

    # setup/build time dependencies listed in pyproject.toml
    # cf. PEP 518
    #setup_requires = ['setuptools_dso'],
    # also need at runtime for DSO filename lookup
    install_requires = [
        'setuptools_dso>=2.7a1',
        epicscorelibs.version.abi_requires(),
    ],
    packages=['pvxslibs', 'pvxslibs.lib', 'pvxslibs.test'],
    package_dir={'': 'python'},
    x_dsos = define_DSOS,
    cmdclass = {
        'build_expand': Expand,
        'install_epics_headers':InstallHeaders,
    },
    zip_safe = False
)
