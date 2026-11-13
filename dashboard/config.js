// Dashboard configuration. Nothing here is secret, and nothing secret
// should ever be put here: anything shipped to a browser is readable by
// whoever opens the page. On a private broker, give the dashboard its own
// READ-ONLY account (an ACL that allows subscribe to the topics below and
// nothing else), so a leaked dashboard login can't publish or control
// devices.
window.DASHBOARD_CONFIG = {
  brokerUrl: "wss://broker.hivemq.com:8884/mqtt",   // MQTT over secure WebSockets
  topicBase: "chinmaydubey231/night-lamp-iot",
  username: "",     // read-only dashboard user on a private broker
  password: "",
  heartbeatMs: 60000, // must match HEARTBEAT_MS in the firmware
};
