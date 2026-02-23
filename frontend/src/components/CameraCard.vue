<template>
  <v-card class="mb-3">
    <v-img :src="imgSrc" :aspect-ratio="camera.currentResolution.width / camera.currentResolution.height" cover
      :id="`cam-${props.camNum}-v-img`" crossorigin="anonymous" @error="onImageError">
      <template #placeholder>
        <div class="d-flex align-center justify-center fill-height">
          <v-progress-circular color="grey-lighten-4" indeterminate />
        </div>
      </template>
      <template #error>
        <div class="d-flex align-center justify-center fill-height">
          <div style="height: 100px;" class="d-flex flex-column">
            <div class="my-auto" style="font-size: larger; font-weight: bold;">
              Something went wrong
            </div>
            <div class="mx-auto" style="font-size: smaller; opacity: 70%;">
              Retrying in 3 seconds...
            </div>
          </div>
        </div>
      </template>
    </v-img>
    <div class="actions">
      <v-btn
        class="action-btn"
        size="x-large"
        variant="tonal"
        @click="captureFrame"
        :prepend-icon="mdiCameraOutline"
        aria-label="Take Snapshot"
      >
        Take Snapshot
      </v-btn>
      <v-btn
        class="action-btn"
        size="x-large"
        variant="outlined"
        href="/snapshots"
        target="_blank"
        rel="noopener noreferrer"
      >
        Open Gallery
      </v-btn>
    </div>
  </v-card>

  <v-snackbar v-model="saveStatusSnackbar" :timeout="2000" color="success">
    {{ saveStatusSnackbarText }}
  </v-snackbar>
</template>

<script setup lang="ts">
import { ref } from 'vue'
import { mdiCameraOutline } from '@mdi/js';
import { useMainStore } from '@/store/mainstore';

const LOADING_IMAGE_SRC = "/loading.jpg"

const mainStore = useMainStore()

const props = defineProps<{
  camNum: number,
}>()

const camera = computed(() => mainStore.clientCameras[props.camNum])

const imgSrc = ref<string>(LOADING_IMAGE_SRC)
const saveStatusSnackbar = ref<boolean>(false)
const saveStatusSnackbarText = ref<string>("")
const retryTimeoutId = ref<ReturnType<typeof setTimeout> | null>(null)
const reloadTimeoutId = ref<ReturnType<typeof setTimeout> | null>(null)

const onImageError = () => {
  if (imgSrc.value === LOADING_IMAGE_SRC) return;

  if (retryTimeoutId.value) {
    clearTimeout(retryTimeoutId.value)
  }

  retryTimeoutId.value = setTimeout(() => {
    reloadCameraSrc(1000)
    retryTimeoutId.value = null
  }, 3000)
}

const realCameraUrl = computed(() => {
  let port: number | null = null;
  let path: string | null = null;

  if (camera.value.src.startsWith(':')) {
    const [, portStr, pathStr] = camera.value.src.split(/[:\/]/);
    port = Number(portStr);
    path = pathStr;

    const realUrl = new URL(path, location.href);
    realUrl.port = port.toString();
    return realUrl.toString();
  } else {
    path = camera.value.src;
    return new URL(path, location.href).toString();
  }
})

const reloadCameraSrc = (ms: number = 100) => {
  imgSrc.value = LOADING_IMAGE_SRC;

  if (reloadTimeoutId.value) {
    clearTimeout(reloadTimeoutId.value)
    reloadTimeoutId.value = null
  }

  reloadTimeoutId.value = setTimeout(() => {
    imgSrc.value = realCameraUrl.value;
    reloadTimeoutId.value = null;
  }, ms);
}

const captureFrame = async () => {
  const url = `/api/capture_image?source=${camera.value.index}`;

  try {
    const res = await fetch(url, { cache: 'no-store' });
    saveStatusSnackbar.value = true;
    saveStatusSnackbarText.value = res.ok ? "Snapshot saved" : "Snapshot failed";
  } catch {
    saveStatusSnackbar.value = true;
    saveStatusSnackbarText.value = "Snapshot failed";
  }
}

watch(realCameraUrl, (newUrl) => {
  if (imgSrc.value !== LOADING_IMAGE_SRC) {
    imgSrc.value = newUrl;
  }
})

onMounted(() => {
  reloadCameraSrc();
})

onUnmounted(() => {
  if (retryTimeoutId.value) {
    clearTimeout(retryTimeoutId.value)
    retryTimeoutId.value = null
  }
})
</script>

<style scoped>
.actions {
  display: flex;
  gap: 10px;
  margin: 12px;
}

.action-btn {
  flex: 1;
  min-height: 52px;
}
</style>
