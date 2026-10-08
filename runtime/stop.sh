#!/bin/sh
killall imx415_dual_person_demo 2>/dev/null || true
pkill -f '[d]ual-person-hdmi.py' 2>/dev/null || true
echo 'Dual-person demo stopped.'
