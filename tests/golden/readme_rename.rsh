# SPDX-License-Identifier: GPL-2.0-or-later
# README.md, "A file name with a space": the snippet as printed there (tests/run.sh checks it is unchanged).
# rsh: each value is one argument
Dir.glob("pics/*.JPG").each do |photo|
  mv #{photo} #{photo.sub(/\.JPG\z/, ".jpg")}
end
puts Dir.children("pics").sort
