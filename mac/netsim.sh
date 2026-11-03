#!/bin/bash
# Simulate a bad network between the Mac and the Frame (all UDP to/from the Frame, both directions) using macOS dummynet.
#   sudo ./netsim.sh start <delay_ms> <loss 0..1>     e.g.  start 40 0.03
#   sudo ./netsim.sh stop
# Always run "stop" afterwards: it flushes the rules and restores the stock pf configuration.
FRAME=${FRAME:?set FRAME to the Steam Frame IP address, e.g. FRAME=192.168.1.50 sudo -E ./netsim.sh start 40 0.03}
case "$1" in
  start)
    DELAY=${2:-30}; LOSS=${3:-0.02}
    dnctl -q flush
    dnctl pipe 1 config delay ${DELAY}ms plr ${LOSS}
    dnctl pipe 2 config delay ${DELAY}ms plr ${LOSS}
    ( cat /etc/pf.conf; echo "dummynet-anchor \"netsim\""; echo "anchor \"netsim\"" ) > /tmp/netsim-pf.conf
    printf "dummynet out proto udp from any to $FRAME pipe 1\ndummynet in proto udp from $FRAME to any pipe 2\n" > /tmp/netsim-rules.conf
    pfctl -q -f /tmp/netsim-pf.conf 2>&1 | grep -v "ALTQ" 
    pfctl -q -a netsim -f /tmp/netsim-rules.conf
    pfctl -q -e 2>/dev/null
    echo "netsim ON: delay ${DELAY}ms, loss ${LOSS} on Mac -> $FRAME:5000" ;;
  stop)
    pfctl -q -a netsim -F all 2>/dev/null
    dnctl -q flush
    pfctl -q -f /etc/pf.conf 2>&1 | grep -v "ALTQ"
    pfctl -q -d 2>/dev/null
    rm -f /tmp/netsim-pf.conf /tmp/netsim-rules.conf
    echo "netsim OFF" ;;
  *) echo "usage: sudo $0 start <delay_ms> <loss 0..1> | stop"; exit 2 ;;
esac
