#!/bin/sh

. "${TEST_SCRIPTS_DIR}/unit.sh"

define_test "eventscript directory with random files"

setup_eventd

touch "$eventd_scriptdir/README.script"

cat > "$eventd_scriptdir/a.script" <<EOF
#!/bin/sh

exit 1
EOF

required_result 0 <<EOF
No event scripts found
EOF
simple_test script list

required_result 22 <<EOF
Script name README is invalid
EOF
simple_test script enable README

required_result 22 <<EOF
Script name a is invalid
EOF
simple_test script disable a

required_result 2 <<EOF
Script 00.test does not exist
EOF
simple_test script enable 00.test

required_result 0 <<EOF
EOF
simple_test run monitor 30

required_result 0 <<EOF
EOF
simple_test status monitor lastrun

required_result 0 <<EOF
EOF
simple_test status monitor lastpass

required_result 0 <<EOF
Event monitor has never failed
EOF
simple_test status monitor lastfail
