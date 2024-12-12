Command Line Tools
==================

A basic set of command line tools are currently provided to facilitate testing and development.
End users should prefer the CLI tools from the `pvAccessCPP <https://github.com/epics-base/pvAccessCPP>`_ module
for day to day use.

* ``pvxcall`` - analogous to ``pvcall``
* ``pvxget`` - analogous to ``pvget``
* ``pvxinfo`` - analogous to ``pvinfo``
* ``pvxmonitor`` - analogous to ``pvmonitor`` or ``pvget -m``
* ``pvxput`` - analogous to ``pvput``
* ``pvxvct`` - UDP search/beacon Troubleshooting tool.

Troubleshooting with Virtual Cable Tester
-----------------------------------------

The ``pvxvct`` executable is capable of listening for UDP searches from PVA clients,
and/or UDP beacons from PVA servers.
Together with ``pvxget`` they can be used to investigate communications issues.

On the host with the PVA server (IOC or otherwise),
run the following to listen for searches. ::

    $ pvxvct -C -P my:random:test:pvname

While this is running, switch to the host where the PVA client resides
and run ::

    $ pvxget my:random:test:pvname

If all goes well, the pvxvct process should print several lines
as search requests are received. eg. ::

    $ pvxvct -C -P my:random:test:pvname
    2020-04-09T19:37:01.146272170 INFO pvxvct 192.168.1.1:47357 Searching for:
    2020-04-09T19:37:01.146442772 INFO pvxvct   "my:random:test:pvname"
    ...

Note that pvxvct does not use the ``$EPICS_PVA*`` environment variables
and by default listens on ``0.0.0.0:5076``.  Sites using a non-default
port will need to add ``-B 0.0.0.0:<port>``.

If searches are not seen, then investigate client :ref:`clientconf`
(``$EPICS_PVA*`` environment variables), and firewall settings.

If searches are seen, then switch to ``pvxget -d ...`` and a real PV name.
The output will be very verbose.  Look for lines like the following: ::

    $ pvxget -d my:real:pv:name
    ...
    2020-04-09T19:44:46.064937960 DEBUG pvxs.client.io UDP search Rx 53 from 192.168.1.1:5076
    2020-04-09T19:44:46.064947396 DEBUG pvxs.client.io Search reply for my:real:pv:name
    2020-04-09T19:44:46.065151400 DEBUG pvxs.client.io Connecting to 192.168.1.1:5075
    2020-04-09T19:44:46.065200101 DEBUG pvxs.client.io Connected to 192.168.1.1:5075
    ...
    2020-04-09T19:44:46.067255960 DEBUG pvxs.client.io Server 192.168.1.1:5075 accepts auth

Repeat with ``pvxinfo`` in place of ``pvxget``.

If the ``...accepts auth...`` line is seen, but no subsequent error message,
then see :ref:`reportbug` and attach the output of ``pvxget -d ...``.
