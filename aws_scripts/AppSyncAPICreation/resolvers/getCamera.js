/**
 * Starts the resolver execution
 * @param {import('@aws-appsync/utils').Context} ctx the context
 * @returns {*} the return value sent to the first AppSync function
 */
export function request(ctx) {
  ctx.stash.thingName = ctx.args.thingName;
  return {};
}


/**
 * Returns the resolver result
 * @param {import('@aws-appsync/utils').Context} ctx the context
 * @returns {*} the return value of the last AppSync function response handler
 */
export function response(ctx) {
  const nowMs = util.time.nowEpochMilliSeconds();
  const argThingName = ctx.stash.thingName;

  const status = ctx.stash.statusItem || null;
  const info = ctx.stash.infoItem || null;

  // If neither exists, return null
  if (!status && !info) return null;

  // Merge with status having precedence over info
  const merged = { ...(info ?? {}), ...(status ?? {}) };

  // Derive thingName from PK if needed
  let derivedThingName = merged.thingName;

  if (!derivedThingName) {
    const pk = merged.PK || status?.PK || info?.PK;
    if (typeof pk === "string" && pk.startsWith("Camera#")) {
      derivedThingName = pk.slice("Camera#".length);
    }
  }

  const thingName = derivedThingName ?? argThingName;
  const online = merged.lastSeen != null ? (nowMs - merged.lastSeen) <= 30_000 : false;
  const sntpSynchronized = merged.driftSec < 31;

  return {
    thingName,

    // status fields
    online,
    recording: merged.recording ?? false,
    lastSeen: merged.lastSeen,
    driftSec: merged.driftSec,
    sntpSynchronized,

    sdPresent: merged.sdPresent,
    sdSizeMB: merged.sdSizeMB,
    sdFreeMB: merged.sdFreeMB,

    // info fields
    firmwareVersion: merged.firmwareVersion,
    sensorModel: merged.sensorModel,
  };
}
