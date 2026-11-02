# Manual redirection test: run inside Terminator with: source tests/test_redirection.sh
echo first > /tmp/terminator-redirection-test.txt
echo second >> /tmp/terminator-redirection-test.txt
cat /tmp/terminator-redirection-test.txt
ls /this-path-should-not-exist 2> /tmp/terminator-redirection-error.txt
cat /tmp/terminator-redirection-error.txt