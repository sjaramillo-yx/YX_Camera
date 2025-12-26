import { util } from "@aws-appsync/utils";

export function request(ctx) {
  const thingName = ctx.stash.thingName;
  return {
    operation: "GetItem",
    key: util.dynamodb.toMapValues({
      PK: `Camera#${thingName}`,
      SK: "info",
    }),
  };
}

export function response(ctx) {
  ctx.stash.infoItem = ctx.result ?? null;
  return ctx.result;
}