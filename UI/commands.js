const names = new Set(["conf", "sniff", "collect", "discover", "status", "calibrate", "diagnose"]);
function parseCommand(body, json = false) {
  let value;
  if (json) value = JSON.parse(body);
  else {
    const [command, first, second, ...extra] = body.trim().split(/\s+/);
    if (extra.length) throw new Error("Too many arguments.");
    if (["conf", "status"].includes(command) && first) throw new Error("This command takes no arguments.");
    value = { command, params: ["sniff","collect","discover"].includes(command) ? { mac: first, channel: second === undefined ? 0 : Number(second) }
      : { board: Number(first), distance: second === undefined ? 1 : Number(second) } };
  }
  if (!value || !names.has(value.command)) throw new Error("Unknown command.");
  const params = value.params || {};
  if (["sniff","collect","discover"].includes(value.command)) {
    const mac = String(params.mac || "").replace(/[:-]/g, "").toUpperCase();
    const channel = params.channel === undefined ? 0 : params.channel;
    if (!/^[0-9A-F]{12}$/.test(mac) || !Number.isInteger(channel) || channel < 0 || channel > 13)
      throw new Error("Enter a 12-digit Wi-Fi MAC and channel 0..13 (0 is automatic).");
    if (value.command==="collect" && channel===0) throw new Error("Lock a channel before collection.");
    if (value.command==="discover" && channel!==0) throw new Error("Discovery uses channel zero.");
    if (params.mlDemo !== undefined && (value.command !== "sniff" || typeof params.mlDemo !== "boolean"))
      throw new Error("The synthetic ML option is a boolean for Sniff only.");
    return { command: value.command, params: { mac, channel, ...(params.mlDemo ? { mlDemo: true } : {}) } };
  }
  if (["calibrate", "diagnose"].includes(value.command)) {
    const distance = params.distance === undefined ? 1 : params.distance;
    if (!Number.isInteger(params.board) || params.board < 2 || params.board > 15 ||
        !Number.isInteger(distance) || distance < 1 || distance > 5)
      throw new Error("Select board 02..15 and distance 1..5 metres.");
    return { command: value.command, params: { board: params.board, distance } };
  }
  return { command: value.command, params: {} };
}
function parseControl(body, activeId, command) {
  const value = JSON.parse(body);
  if (value.commandId !== activeId || !activeId) throw new Error("The command is no longer active.");
  const action = value.action;
  const allowed = action === "cancel" || command === "calibrate" &&
    (["next", "save", "retry"].includes(action) || /^remeasure [1-5]$/.test(action)) ||
    command === "diagnose" && action === "next";
  if (!allowed) throw new Error("That action is not supported for this command.");
  return action;
}
module.exports = { parseCommand, parseControl, names };
