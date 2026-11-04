# docs/rsh.md and rsh.1, "Sh": the example, line for line (logs/ is made by doc_sh.setup).
# pack the logs older than a week, then say what is left
old = Sh.find("logs", name: "*.log", older: 7 * 86400)
if old.any?
  tar -czf old-logs.tgz #{old}
  Sh.rm(old)
end
puts "packed #{old.size}, kept #{Sh.find("logs", type: :file).size}"
Sh.tmpdir do |dir|
  Sh.cp_r("logs", dir)
  puts Sh.grep("ok", Sh.find(dir, name: "*.log"))
end
abort "rsync is missing" unless Sh.which("rsync")
