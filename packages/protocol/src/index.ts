export type PeerState = {
  microphone: boolean;
  camera: boolean;
  screenShare: boolean;
  cameraStreamId?: string;
  screenStreamId?: string;
  /** Segunda transmissão de tela (outro monitor). Clientes antigos ignoram o campo. */
  secondScreenStreamId?: string;
  screenAudio?: boolean;
};

export type IceCandidatePayload = {
  candidate: string;
  sdpMid: string | null;
  sdpMLineIndex: number | null;
  usernameFragment?: string | null;
};
