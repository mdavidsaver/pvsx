/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * pvxs is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */

#include "cliutil.h"

namespace pvxs {

bool operator==(const ArgVal& rhs, const ArgVal& lhs) {
    return rhs.defined==lhs.defined && rhs.value==lhs.value;
}

GetOpt::GetOpt(int argc, char *argv[], const char *spec)
    :argv0("<program name>")
{
    if(argc>=1)
        argv0 = argv[0];

    bool allpos = false; // after "--", treat all remaining as positional
    for(int i=1; i<argc; i++) {
        const char * arg = argv[i];
        if(!allpos && arg[0]=='-') {
            arg++;

            if(arg[1]=='-') {
                if(arg[2]=='\0') { // "--"
                    allpos = true;
                    continue;
                }
                // "--..." not supported
                arguments.emplace_back(-1, argv[i]);
                return;
            }
            // process as short args

            for(; *arg; arg++) {
                for(auto s=spec; *s; s++) {
                    if(*s==*arg) { // match
                        if(s[1]==':') { // need arg value
                            if(arg[1]=='\0') { // "-a", "value"
                                if(i+1==argc) {
                                    // oops. no value
                                    arguments.emplace_back('?', nullptr);
                                    return;
                                }
                                arguments.emplace_back(*arg, argv[i+1]);
                                i++;

                            } else {
                                // "-avalue"
                                arguments.emplace_back(*arg, &arg[1]);
                            }
                            goto nextarg;

                        } else { // flag
                            arguments.emplace_back(*s, nullptr);
                            // continue scanning for more flags.  eg. "-vv"
                            goto nextchar;
                        }
                    } else {
                        if(s[1]==':')
                            s++;
                    }
                }
                // unrecognized
                arguments.emplace_back(-1, nullptr);
                return;
nextchar:
                (void)0;  // need a statement after label...
            }
nextarg:
            (void)0;

        } else {
            positional.push_back(arg);
        }
        success = true;
    }
}

} // namespace pvxs
