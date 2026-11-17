#!/usr/bin/env bash
# Deploy the dashboard to Google Cloud Run. Reads credentials from dashboard/.env.
set -euo pipefail

export PATH="/Applications/google-cloud-sdk/bin:${PATH}"
APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ -f "${APP_DIR}/.env" ]]; then
  set -a
  # shellcheck disable=SC1091
  source "${APP_DIR}/.env"
  set +a
fi

PROJECT_ID="${PROJECT_ID:-watdoi-monitoring}"
REGION="${REGION:-asia-southeast1}"
SERVICE_NAME="${SERVICE_NAME:-phrajd-watering}"
: "${MQTT_URL:?MQTT_URL is required (set it in .env)}"
: "${MQTT_USERNAME:?MQTT_USERNAME is required}"
: "${MQTT_PASSWORD:?MQTT_PASSWORD is required}"
: "${CONTROL_PASSWORD:?CONTROL_PASSWORD is required}"
: "${MQTT_VIEW_USERNAME:?MQTT_VIEW_USERNAME is required (subscribe-only EMQX account)}"
: "${MQTT_VIEW_PASSWORD:?MQTT_VIEW_PASSWORD is required}"
: "${LOG_TOKEN:?LOG_TOKEN is required (same value as LOG_TOKEN in include/secrets.h)}"
MQTT_WS_URL="${MQTT_WS_URL:-}"

echo "Project: ${PROJECT_ID}  Region: ${REGION}  Service: ${SERVICE_NAME}"

gcloud services enable run.googleapis.com secretmanager.googleapis.com \
  cloudbuild.googleapis.com artifactregistry.googleapis.com firestore.googleapis.com --project "${PROJECT_ID}"

# Logs live in the (default) Firestore database, the only one covered by the free tier.
if ! gcloud firestore databases describe --database='(default)' --project "${PROJECT_ID}" >/dev/null 2>&1; then
  gcloud firestore databases create --database='(default)' --location="${REGION}" \
    --type=firestore-native --project "${PROJECT_ID}"
fi
gcloud firestore fields ttls update expireAt --collection-group=wateringLogs \
  --enable-ttl --async --project "${PROJECT_ID}" --quiet >/dev/null

# Creates the secret, or adds a new version when the value changed and destroys the old ones:
# only 6 secret versions per billing account are free and disabled versions still count.
upsert_secret() {
  local name="$1" value="$2"
  if ! gcloud secrets describe "${name}" --project "${PROJECT_ID}" >/dev/null 2>&1; then
    printf '%s' "${value}" | gcloud secrets create "${name}" --project "${PROJECT_ID}" \
      --replication-policy=automatic --data-file=-
  elif [[ "$(gcloud secrets versions access latest --secret "${name}" --project "${PROJECT_ID}")" != "${value}" ]]; then
    printf '%s' "${value}" | gcloud secrets versions add "${name}" --project "${PROJECT_ID}" --data-file=-
    local latest
    latest="$(gcloud secrets versions list "${name}" --project "${PROJECT_ID}" --sort-by=~createTime --limit=1 --format='value(name)')"
    for old in $(gcloud secrets versions list "${name}" --project "${PROJECT_ID}" --filter='state!=DESTROYED' --format='value(name)'); do
      [[ "${old}" == "${latest}" ]] || gcloud secrets versions destroy "${old}" --secret "${name}" --project "${PROJECT_ID}" --quiet >/dev/null
    done
  fi
}

# Separate from mqtt-password, which wat-doi-dashboard still uses for HiveMQ.
upsert_secret phrajd-mqtt-password "${MQTT_PASSWORD}"
upsert_secret watering-control-password "${CONTROL_PASSWORD}"

PROJECT_NUMBER="$(gcloud projects describe "${PROJECT_ID}" --format='value(projectNumber)')"
gcloud projects add-iam-policy-binding "${PROJECT_ID}" \
  --member="serviceAccount:${PROJECT_NUMBER}-compute@developer.gserviceaccount.com" \
  --role='roles/secretmanager.secretAccessor' --condition=None --quiet >/dev/null
gcloud projects add-iam-policy-binding "${PROJECT_ID}" \
  --member="serviceAccount:${PROJECT_NUMBER}-compute@developer.gserviceaccount.com" \
  --role='roles/datastore.user' --condition=None --quiet >/dev/null

# Browsers read status directly from EMQX, so the service only runs while handling a request
# (scale to zero, CPU billed only during requests) and stays within the Cloud Run free tier.
# The view password is handed to every browser anyway and LOG_TOKEN only guards log uploads,
# so both are plain env vars to keep Secret Manager within its 6 free versions.
gcloud run deploy "${SERVICE_NAME}" \
  --source "${APP_DIR}" \
  --region "${REGION}" \
  --project "${PROJECT_ID}" \
  --allow-unauthenticated \
  --min-instances=0 --max-instances=1 --cpu-throttling \
  --memory=256Mi \
  --set-env-vars="^@^MQTT_URL=${MQTT_URL}@MQTT_WS_URL=${MQTT_WS_URL}@MQTT_USERNAME=${MQTT_USERNAME}@MQTT_VIEW_USERNAME=${MQTT_VIEW_USERNAME}@MQTT_VIEW_PASSWORD=${MQTT_VIEW_PASSWORD}@LOG_TOKEN=${LOG_TOKEN}" \
  --set-secrets="MQTT_PASSWORD=phrajd-mqtt-password:latest,CONTROL_PASSWORD=watering-control-password:latest"

URL="$(gcloud run services describe "${SERVICE_NAME}" --region "${REGION}" --project "${PROJECT_ID}" --format='value(status.url)')"
printf '\nDeployed: %s\n' "${URL}"
