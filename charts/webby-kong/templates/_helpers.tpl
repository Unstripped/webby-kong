{{/*
Expand the name of the chart.
*/}}
{{- define "webby-kong.name" -}}
{{- default .Chart.Name .Values.nameOverride | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
Create a default fully qualified app name.
We truncate at 63 chars because some Kubernetes name fields are limited to this (by the DNS naming spec).
If release name contains chart name it will be used as a full name.
*/}}
{{- define "webby-kong.fullname" -}}
{{- if .Values.fullnameOverride }}
{{- .Values.fullnameOverride | trunc 63 | trimSuffix "-" }}
{{- else }}
{{- $name := default .Chart.Name .Values.nameOverride }}
{{- if contains $name .Release.Name }}
{{- .Release.Name | trunc 63 | trimSuffix "-" }}
{{- else }}
{{- printf "%s-%s" .Release.Name $name | trunc 63 | trimSuffix "-" }}
{{- end }}
{{- end }}
{{- end }}

{{/*
Create chart name and version as used by the chart label.
*/}}
{{- define "webby-kong.chart" -}}
{{- printf "%s-%s" .Chart.Name .Chart.Version | replace "+" "_" | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
Common labels
*/}}
{{- define "webby-kong.labels" -}}
helm.sh/chart: {{ include "webby-kong.chart" . }}
{{ include "webby-kong.selectorLabels" . }}
{{- if .Chart.AppVersion }}
app.kubernetes.io/version: {{ .Chart.AppVersion | quote }}
{{- end }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
{{- end }}

{{/*
Selector labels
*/}}
{{- define "webby-kong.selectorLabels" -}}
app.kubernetes.io/name: {{ include "webby-kong.name" . }}
app.kubernetes.io/instance: {{ .Release.Name }}
{{- end }}

{{/*
Container image; the tag defaults to the chart's appVersion (the release tag).
*/}}
{{- define "webby-kong.image" -}}
{{ .Values.image.repository }}:{{ .Values.image.tag | default .Chart.AppVersion }}
{{- end }}

{{/*
Secret holding the SMTP password: the user's existingSecret, else the chart-created one.
*/}}
{{- define "webby-kong.smtpSecretName" -}}
{{- if .Values.smtp.existingSecret }}
{{- .Values.smtp.existingSecret }}
{{- else }}
{{- include "webby-kong.fullname" . }}
{{- end }}
{{- end }}

{{- define "webby-kong.smtpPasswordKey" -}}
{{- if .Values.smtp.existingSecret }}
{{- .Values.smtp.existingSecretPasswordKey }}
{{- else -}}
smtp-password
{{- end }}
{{- end }}

{{/*
"true" when the chart creates the SMTP password Secret (empty otherwise).
*/}}
{{- define "webby-kong.smtpSecretEnabled" -}}
{{- if and .Values.auth.enabled .Values.smtp.password (not .Values.smtp.existingSecret) -}}
true
{{- end }}
{{- end }}
