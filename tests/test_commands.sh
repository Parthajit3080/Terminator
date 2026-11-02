# Manual smoke test: run inside Terminator with: source tests/test_commands.sh
printf 'commands: begin\n'
echo Terminator command test
pwd
cd /tmp
pwd
cd -
history
printf 'commands: end\n'