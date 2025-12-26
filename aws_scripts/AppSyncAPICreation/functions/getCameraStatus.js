import { util } from "@aws-appsync/utils";

export function request(ctx) {
  const thingName = ctx.stash.thingName;
  return {
    operation: "GetItem",
    key: util.dynamodb.toMapValues({
      PK: `Camera#${thingName}`,
      SK: "status",
    }),
  };
}

export function response(ctx) {
  ctx.stash.statusItem = ctx.result ?? null;
  return ctx.result;
}
