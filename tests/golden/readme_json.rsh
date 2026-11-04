# SPDX-License-Identifier: GPL-2.0-or-later
# README.md, "JSON from a web service": the snippet as printed there (tests/run.sh checks it is unchanged).
# rsh: JSON is built in
require "json"
issues = JSON.parse(`curl -fsS https://api.github.com/repos/wonderlandos/rsh/issues`)
bugs = issues.select { |i| i["labels"].any? { |l| l["name"] == "bug" } }
bugs.each { |i| puts "##{i["number"]} #{i["title"]}" }
