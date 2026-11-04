#!/usr/bin/rsh
dest = ARGV[0] || "/srv/app"
cd ~/src/app
git pull --ff-only && make -j4
rsync -a build/ #{dest}/
errors = `journalctl -b -p err`.lines.reject { |l| l =~ /fwupd/ }
puts "#{errors.size} errors" unless errors.empty?
