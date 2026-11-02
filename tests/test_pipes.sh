# Manual pipeline test: run inside Terminator with: source tests/test_pipes.sh
echo alpha | tr a-z A-Z
echo 'first second third' | wc -w
printf 'red\nblue\n' | grep blue