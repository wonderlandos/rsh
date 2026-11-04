# docs/rsh.md, "What it is": the deploy example, line for line after these three lines (the stand-ins
# in tests/fakebin print what they are given; nprocessors is fixed so the output is the same everywhere).
module Etc; def self.nprocessors = 4; end
#!/usr/bin/rsh
# SPDX-License-Identifier: GPL-2.0-or-later
# deploy: build and copy the app, restart it if it runs.
dest = ARGV[0] || "/srv/app"
cd ~/src/app
git pull --ff-only && make -j#{Etc.nprocessors}
rsync -a build/ #{dest}/            # dest is one argument, whatever it contains
if systemctl is-active --quiet app
  systemctl --user restart app
end

