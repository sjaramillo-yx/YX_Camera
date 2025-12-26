import { util } from "@aws-appsync/utils";

export function request(ctx) {
  const args = ctx.args ?? ctx.arguments ?? {};
  const limit = args.limit ?? 50;
  const nextToken = args.nextToken;

  return {
    operation: "Query",
    index: "all-recordings",
    query: {
      expression: "#pk = :pk",
      expressionNames: { "#pk": "allRecordingsPk" },
      expressionValues: util.dynamodb.toMapValues({ ":pk": "Recs#ALL" }),
    },
    limit,
    nextToken,
    scanIndexForward: false, // newest first
  };
}

export function response(ctx) {
  const { items = [], nextToken = null } = ctx.result ?? {};

  const mapped = items.map((it) => ({
    transactionId: it.transactionId,
    status: it.status ?? "UNKNOWN",

    hres: it.hres,
    vres: it.vres,

    targetFPS: it.targetFps,
    targetBitrate: it.targetBitrate,

    currentFPS: it.currentFPS,
    currentBitrate: it.currentBitrate,

    lengthSeconds: it.lengthSeconds ?? it.duration ?? it.recordedSeconds ?? it.lengthSec,

    filename: it.filename,
    filesize: it.filesize,

    updatedAt: it.updatedAt,

    errorCode: it.errorCode,
    errorMessage: it.errorMessage,
    failedModule: it.failedModule,
    errorTimestamp: it.errorTimestamp,
  }));

  return { items: mapped, nextToken };
}