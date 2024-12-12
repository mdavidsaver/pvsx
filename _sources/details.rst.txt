*******
Details
*******

.. _reportbug:

Reporting a Bug
===============

Before reporting a bug, please check to see if this issue has already been `reported <https://github.com/epics-base/pvxs/issues>`_.

When composing a new report, please run the included automatic tests "make runtests" and mention the results.
It is enough to mention "All tests successful." if this is so.  (see `runtests`)

`Bug reports <https://github.com/epics-base/pvxs/issues>`_ should always include:

* EPICS Base version or VCS commit
* PVXS module version or VCS commit
* libevent version or VCS commit (running "pvxget -V" shows these)
* EPICS host and target archs.  (eg. "linux-x86_64")
* Host OS name and version (eg. run "cat /etc/issue")
* Compiler name and version (eg. run "gcc --version")
* The values of any $EPICS_PVA* environment variables which are set.
* Any local/site modifications to PVXS or libevent
* Concise instructions for reproducing the issue.

Additional information which may be relevant:

* Number of network interfaces if more than one.
* Whether clients and/or servers are on the same host or different hosts.
* Whether clients and/or servers are in the same subnet or different subnets
* Whether network traffic crosses between virtual machine(s) and physical host(s).
* Firewall rules on UDP traffic to/from port 5075 or TCP connections to port 5074
* Any local/site modifications to EPICS Base

If the module has built successfully, then running ``pvxinfo -D`` will
report much of this information.

Packet Capture
--------------

Some kinds of issues can be easily reproduced by the reporter, but not by others.
eg. those related to interoperability, site private applications, or "environmental" conditions.
In these cases it may be necessary to capture the troublesome network traffic to file.
This section provides a quick guide for using `Wireshark <http://www.wireshark.org/>`_ to do this.

An important concern when providing a packet capture taken on a private network is to
avoid unintentionally capturing unrelated, and potentially sensitive, traffic.

Usage of the ``pva.lua`` extension found in `cashark <https://github.com/mdavidsaver/cashark>`_
provides a quick way to filter out non-PVA traffic. ::

    wireshark -X lua_script:pva.lua

While Wireshark is running, apply a display filter of ``pva`` to show only PV Access protocol traffic.
From the ``File`` menu, select ``Export Specified Packets``, then choose to export only displayed packets.
Please also check ``Compress with gzip``.

If it is necessary to select only a specific PVA exchange, use the
`Follow TCP Stream <https://www.wireshark.org/docs/wsug_html_chunked/ChAdvFollowStreamSection.html>`_
feature to display/export only packets associated with a single TCP connection.
This will result in a display filter expression like ``tcp.stream eq 1``.
When trying to identify a single connection/stream, it may be helpful to apply a filter like ``pva.pv=="special:pv:name"``.

If if isn't convenient to run the full Wireshark GUI when capturing,
the ``tshark`` CLI utility may be used to capture raw traffic to file.
Display filters like ``pva.lua`` may not be used during capture,
so it will be necessary to filter again using the GUI on another host to produce a second (cooked) capture file. ::

    tshark -i any -f '!port ssh' -w raw.pcapng
    gzip raw.pcapng
    # copy raw.pcapng.gz to another host
    wireshark -X lua_script:pva.lua raw.pcapng.gz

If is doubt about whether private information has been successfully excluded,
packet capture files may be sent privately to the author by email instead of uploading to github.
The author's PGP key may be found below.

Please **compress** all capture files uploaded or sent!

.. _relpolicy:

Release Policy
==============

PVXS Release numbering follows the `Semantic Versioning <https://semver.org/>`_
scheme of MAJOR.MINOR.PATCH with the following amendments.

* A change to the MAJOR number indicates that a backwards incompatible change to some part of the public API.
  This may not effect every user application.
  This policy is intended to provide users with confidence in upgrading when MAJOR does not change.
* The PATCH number will only be incremented if changes to the public API are believed to maintain ABI compatibility.
  MINOR will be incremented when a known ABI incompatible change is made.
  Library SONAMES take the form MAJOR.MINOR.
* Backwards incompatible changes to semi-public :ref:`expertapi` may appear in a MINOR release.
* At this time only one version number is maintained, which is applied to both
  the main libpvxs.so and the auxiliary libpvxsIoc.so.
  Statements about API or ABI compatibility apply to both libraries as a group.
* See :ref:`ntcompat` for ``NT*`` type construction helpers.

.. _pgpkey:
  
Each release will be accompanied by a signed tag in the git repository,
which may be verified with the author's GPG key
`5C159E669D69E2D4C4E74E540C8E1C8347330CFB <https://keys.openpgp.org/search?q=5C159E669D69E2D4C4E74E540C8E1C8347330CFB>`_
`(alternate) <https://keyserver.ubuntu.com/pks/lookup?search=5C159E669D69E2D4C4E74E540C8E1C8347330CFB&fingerprint=on&op=index>`_
.

.. _expertapi:

Expert APIs
===========

The Expert API are a set of semi-public definitions and methods which are not intended for general use,
and may be subject to incompatible change in a minor release.
Expert API calls are wrapped by "#ifdef PVXS_EXPERT_API_ENABLED"
to prevent unintentional usage.

If a change is considered,
best effort will be made to involve developers/sites known to make use of Expert API.
Prospective users of the Expert API are encouraged to contact the author.

Elements of the Expert API may be "promoted" to regular/full API status if warranted.

.. _contrib:

Contributing
============

The recommended path for including changes is through `Pull Request <https://github.com/epics-base/pvxs/pulls>`_.

When changing c++ code please do:

* Indent with 4 spaces.  No hard tabs.  UNIX style EoL.
* Try to maintain the style of surrounding code.
* Include meaningful code comments where reasonable.
* Add doxygen tags ``@since UNRELEASED`` when documenting additions/changes to public APIs.

but do not:

* Add any c++ global constructors or destructors in the pvxs library.  (Ok in tools, examples, or tests)

When committing changes please do:

* Include a commit message
* Break up changes into multiple commits where reasonable
* Include whitespace only changes as separate commits

.. _contributors:

Contributors
------------

Who did the [work](https://github.com/epics-base/pvxs/graphs/contributors) to make PVXS what it is.

.. comment: git log --format=format:%aN|sort -u|while read aa; do echo "* $aa"; done

* Alexander Wells
* Basil Aljamal
* Bruno Martins
* Érico Nogueira
* George McIntyre
* Henrique Silva
* karlosp
* Klemen Vodopivec
* Michael Davidsaver
* Peter Milne
* Simon Rose
* Thomas Ives

Those who supported this work.

* [ALS-U](https://als.lbl.gov/als-u/overview/) project at [Berkeley Lab](https://www.lbl.gov/)
* [Diamond Light Source](https://www.diamond.ac.uk/)
* [European Spallation Source](https://europeanspallationsource.se/)
* [Fermilab](https://fnal.gov/)
* [SLAC National Accelerator Laboratory](https://www6.slac.stanford.edu/)
* [SNS](https://neutrons.ornl.gov/sns) at [Oak Ridge National Lab](https://www.ornl.gov/)

Implementation Notes
====================

Misc. notes on design and Implementation.

* All Server and client Context instances listening on the same UDP port# within a process
  will share a single UDP socket.

* The UDP local multicast fanout aspect of the PVA protocol is not implemented.

* Client UDP search retry follows a linear backoff starting from 1 second
  and stepping to 30 seconds.  cf. bucketInterval and nBuckets in client.cpp.

* To level UDP search traffic, search retry may delay a PV for an extra
  bucket if the difference in the number of PVs in the desired and subsequent
  buckets is too large.

* Client Context::hurryUp() expires the search bucket timer immediately,
  saving up to bucketInterval seconds.

* Each Value refers points to a pair of FieldDesc and FieldStorage in arrays
  of the same.  Value::operator[] steps around in these arrays.

* There is a hidden StructTop which holds the FieldStorage array and holds
  a shared_ptr to the FieldDesc array to join ownership of the two.

* TCP connection buffering will read up to tcp_readahead (cf. conn.h) bytes
  while waiting for a complete header.  After a header is received,
  the larger of tcp_readahead or the message body length is buffered.

