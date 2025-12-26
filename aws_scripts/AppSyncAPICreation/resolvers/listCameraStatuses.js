import { util } from "@aws-appsync/utils";

export function request(ctx) {
  const args = ctx.args ?? ctx.arguments ?? {};
  const limit = args.limit ?? 50;
  const nextToken = args.nextToken;

  return {
    operation: "Query",
    index: "all-cameras",
    query: {
      expression: "#pk = :pk",
      expressionNames: { "#pk": "camerasByLastSeenPk" },
      expressionValues: util.dynamodb.toMapValues({ ":pk": "Cameras#ALL" }),
    },
    limit,
    nextToken,
    scanIndexForward: false, // newest first
  };
}

export function response(ctx) {
  const { items = [], nextToken = null } = ctx.result ?? {};

  const nowMs = util.time.nowEpochMilliSeconds();

  const mapped = items.map((it) => {
    // Check if source includes thingName
    let thingName = it.thingName;
    // If not, derive it from camerasByLastSeenSk
    if (!thingName && typeof it.camerasByLastSeenSk === "string") {
      const i = it.camerasByLastSeenSk.indexOf("#");
      if (i >= 0) thingName = it.camerasByLastSeenSk.slice(i + 1);
    }
    // If camerasByLastSeenPk is not a string, obtain it from the table PK
    if (!thingName && typeof it.PK === "string" && it.PK.startsWith("Camera#")) {
      thingName = it.PK.slice("Camera#".length);
    }
    const online = it.lastSeen != null ? (nowMs - it.lastSeen) <= 30_000 : false;
    const sntpSynchronized = it.driftSec < 31;

    return {
      thingName,                    // must be non-null for ID!
      online,
      recording: it.recording ?? false,
      lastSeen: it.lastSeen,
      driftSec: it.driftSec,
      sntpSynchronized,
      sdPresent: it.sdPresent,
      sdSizeMB: it.sdSizeMB,
      sdFreeMB: it.sdFreeMB
    };
  });

  return { items: mapped, nextToken };
}
