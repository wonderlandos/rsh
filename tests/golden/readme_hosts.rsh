# SPDX-License-Identifier: GPL-2.0-or-later
# README.md, "The same check on several computers": the snippet as printed there (tests/run.sh checks it is unchanged).
down = []
%w[web1 web2 db1].each do |host|
  begin
    ssh -o BatchMode=yes #{host} systemctl is-active --quiet nginx
  rescue Rsh::Failed => e
    down << "#{host} (#{e.status.exitstatus})"
  end
end
puts down.empty? ? "all up" : "down: #{down.join(", ")}"
