import React, { useCallback, useEffect, useRef, useState } from "react";
import mpegts from "mpegts.js";
import { Channel, EPGProgram } from "../../types/player";
import { usePlayerTranslation } from "../../hooks/use-player-translation";
import type { Locale } from "../../lib/locale";
import { PlayerControls } from "./player-controls";

interface VideoPlayerProps {
  channel: Channel | null;
  segments: mpegts.MediaSegment[];
  onError?: (error: string) => void;
  locale: Locale;
  currentProgram?: EPGProgram | null;
  onSeek?: (seekTime: Date) => void;
  streamStartTime: Date;
  currentVideoTime: number;
  onCurrentVideoTimeChange: (time: number) => void;
  onPrevChannel?: () => void;
  onNextChannel?: () => void;
  showSidebar?: boolean;
  onToggleSidebar?: () => void;
  onFullscreenToggle?: () => void;
  onSearchInput?: (text: string) => void;
}

export function VideoPlayer({
  channel,
  segments,
  onError,
  locale,
  currentProgram = null,
  onSeek,
  streamStartTime,
  currentVideoTime,
  onCurrentVideoTimeChange,
  onPrevChannel,
  onNextChannel,
  showSidebar = true,
  onToggleSidebar,
  onFullscreenToggle,
  onSearchInput,
}: VideoPlayerProps) {
  const t = usePlayerTranslation(locale);

  const videoRef = useRef<HTMLVideoElement>(null);
  const playerRef = useRef<mpegts.Player | null>(null);
  const [isLoading, setIsLoading] = useState(false);
  const [showLoading, setShowLoading] = useState(false);
  const loadingTimeoutRef = useRef<NodeJS.Timeout | null>(null);
  const [isLive, setIsLive] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [volume, setVolume] = useState(1);
  const [isMuted, setIsMuted] = useState(false);
  const [isPlaying, setIsPlaying] = useState(false);
  const [needsUserInteraction, setNeedsUserInteraction] = useState(false);
  const [showControls, setShowControls] = useState(false);
  const hideControlsTimeoutRef = useRef<NodeJS.Timeout | null>(null);

  // Debounce loading indicator to prevent flickering on fast loads
  useEffect(() => {
    if (isLoading) {
      // Show loading indicator after 200ms delay
      loadingTimeoutRef.current = setTimeout(() => {
        setShowLoading(true);
      }, 500);
    } else {
      // Immediately hide loading indicator
      if (loadingTimeoutRef.current) {
        clearTimeout(loadingTimeoutRef.current);
        loadingTimeoutRef.current = null;
      }
      setShowLoading(false);
    }

    return () => {
      if (loadingTimeoutRef.current) {
        clearTimeout(loadingTimeoutRef.current);
        loadingTimeoutRef.current = null;
      }
    };
  }, [isLoading]);

  const handleSeek = useCallback(
    (seekTime: Date) => {
      if (playerRef.current && seekTime.getTime() >= streamStartTime.getTime()) {
        const seekSeconds = (seekTime.getTime() - streamStartTime.getTime()) / 1000;

        // Check if seekSeconds is within buffered ranges
        const buffered = playerRef.current.buffered;
        let isBuffered = false;

        for (let i = 0; i < buffered.length; i++) {
          const start = buffered.start(i);
          const end = buffered.end(i);

          if (seekSeconds >= start && seekSeconds <= end) {
            isBuffered = true;
            break;
          }
        }

        if (isBuffered) {
          // Seek within buffered range
          playerRef.current.currentTime = seekSeconds;
          playerRef.current.play(); // resume if paused
          return;
        }
      }
      // If not buffered, call onSeek callback
      onSeek?.(seekTime);
    },
    [onSeek, streamStartTime],
  );

  const togglePlayPause = useCallback(() => {
    if (playerRef.current && videoRef.current) {
      if (videoRef.current.paused) {
        playerRef.current.play();
        setNeedsUserInteraction(false);
      } else {
        playerRef.current.pause();
      }
    }
  }, []);

  // Keyboard controls
  useEffect(() => {
    const handleKeyDown = (e: KeyboardEvent) => {
      // Ignore if user is typing in an input field
      if (e.target instanceof HTMLInputElement || e.target instanceof HTMLTextAreaElement) {
        return;
      }

      // Check if it's a number key (0-9)
      const isNumberKey = /^[0-9]$/.test(e.key);

      // If it's a number key, focus search and append the number
      if (isNumberKey && onSearchInput) {
        e.preventDefault();
        onSearchInput(e.key);
        return;
      }

      switch (e.key) {
        case "ArrowUp":
          e.preventDefault();
          // Previous channel
          if (onPrevChannel) {
            onPrevChannel();
          }
          break;

        case "ArrowDown":
          e.preventDefault();
          // Next channel
          if (onNextChannel) {
            onNextChannel();
          }
          break;

        case "ArrowLeft": {
          e.preventDefault();
          // Seek backward 5 seconds
          const currentAbsoluteTime = new Date(streamStartTime.getTime() + currentVideoTime * 1000);
          const newSeekTime = new Date(currentAbsoluteTime.getTime() - 5000);
          handleSeek(newSeekTime);
          break;
        }

        case "ArrowRight": {
          e.preventDefault();
          // Seek forward 5 seconds
          const currentAbsoluteTime = new Date(streamStartTime.getTime() + currentVideoTime * 1000);
          const newSeekTime = new Date(currentAbsoluteTime.getTime() + 5000);
          handleSeek(newSeekTime);
          break;
        }

        case " ": // Space
          e.preventDefault();
          togglePlayPause();
          break;

        case "m":
        case "M":
          e.preventDefault();
          // Toggle mute
          setIsMuted((prev) => !prev);
          break;

        case "f":
        case "F":
          e.preventDefault();
          // Toggle fullscreen
          if (onFullscreenToggle) {
            onFullscreenToggle();
          }
          break;

        case "s":
        case "S":
          e.preventDefault();
          // Toggle sidebar
          if (onToggleSidebar) {
            onToggleSidebar();
          }
          break;
      }
    };

    window.addEventListener("keydown", handleKeyDown);
    return () => {
      window.removeEventListener("keydown", handleKeyDown);
    };
  }, [
    handleSeek,
    streamStartTime,
    currentVideoTime,
    onPrevChannel,
    onNextChannel,
    onFullscreenToggle,
    onToggleSidebar,
    onSearchInput,
    togglePlayPause,
  ]);

  // Reset hide timer for controls
  const resetHideTimer = useCallback(() => {
    setShowControls(true);
    if (hideControlsTimeoutRef.current) {
      clearTimeout(hideControlsTimeoutRef.current);
    }
    hideControlsTimeoutRef.current = setTimeout(() => {
      setShowControls(false);
    }, 3000);
  }, []);

  // Handle mouse leave - hide controls immediately
  const handleMouseLeave = useCallback(() => {
    if (hideControlsTimeoutRef.current) {
      clearTimeout(hideControlsTimeoutRef.current);
    }
    setShowControls(false);
  }, []);

  // Handle video click/tap - toggle controls
  const handleVideoClick = useCallback(() => {
    if (showControls) {
      // Hide controls immediately
      if (hideControlsTimeoutRef.current) {
        clearTimeout(hideControlsTimeoutRef.current);
      }
      setShowControls(false);
    } else {
      // Show controls and start auto-hide timer
      resetHideTimer();
    }
  }, [showControls, resetHideTimer]);

  // Start auto-hide timer on mount
  useEffect(() => {
    resetHideTimer();
    return () => {
      if (hideControlsTimeoutRef.current) {
        clearTimeout(hideControlsTimeoutRef.current);
      }
    };
  }, [resetHideTimer]);

  // Initialize mpegts.js
  useEffect(() => {
    if (!mpegts.isSupported()) {
      const errorMsg = t("mseNotSupported");
      setError(errorMsg);
      onError?.(errorMsg);
    }
  }, [onError, t]);

  // Load video stream
  useEffect(() => {
    if (!segments.length || !videoRef.current || !mpegts.isSupported()) return;

    setShowControls(true);
    setIsLoading(true);
    setError(null);
    setIsPlaying(false);
    setNeedsUserInteraction(false);

    try {
      const player = mpegts.createPlayer(
        {
          type: "mpegts",
          segments,
        },
        {
          enableWorker: true,
          isLive: true,
          enableStashBuffer: false,
        },
      );

      playerRef.current = player;
      player.attachMediaElement(videoRef.current);

      player.on(mpegts.Events.ERROR, (errorType, errorDetail, errorInfo) => {
        console.error("Player error:", errorType, errorDetail, errorInfo);
        let errorMessage = t("playbackError");

        if (errorType === mpegts.ErrorTypes.MEDIA_ERROR) {
          if (errorDetail === mpegts.ErrorDetails.MEDIA_FORMAT_UNSUPPORTED || errorInfo?.msg?.includes("codec")) {
            errorMessage = t("codecError");
          } else {
            errorMessage = `${t("mediaError")}: ${errorDetail}`;
          }
        } else if (errorType === mpegts.ErrorTypes.NETWORK_ERROR) {
          errorMessage = `${t("networkError")}: ${errorDetail}`;
        }

        setError(errorMessage);
        onError?.(errorMessage);
        setIsLoading(false);
      });

      player.load();

      const playPromise = player.play();
      if (playPromise) {
        playPromise
          .catch((err: Error) => {
            if (err.name === "NotAllowedError" || err.message.includes("user didn't interact")) {
              setNeedsUserInteraction(true);
            }
          })
          .finally(() => {
            setIsLoading(false);
          });
      }
    } catch (err) {
      const errorMsg = err instanceof Error ? err.message : t("failedToPlay");
      setError(errorMsg);
      onError?.(errorMsg);
      setIsLoading(false);
    }

    return () => {
      if (playerRef.current) {
        playerRef.current.destroy();
        playerRef.current = null;
      }
    };
  }, [segments, onError, t]);

  // Handle video events
  useEffect(() => {
    const video = videoRef.current;
    if (!video) return;

    const handleCanPlay = () => {
      setIsLoading(false);
      setIsPlaying(true);
    };

    const handleWaiting = () => {
      setIsLoading(true);
    };

    const handlePlaying = () => {
      setIsLoading(false);
      setIsPlaying(true);
    };

    const handlePause = () => {
      setIsPlaying(false);
      setIsLive(false);
    };

    const handleTimeUpdate = () => {
      onCurrentVideoTimeChange(video.currentTime);
      setIsLive(streamStartTime.getTime() + video.currentTime * 1000 >= Date.now() - 3000);
    };

    const handleEnded = () => {
      if (onSeek && playerRef.current?.duration) {
        const seekTime = new Date(streamStartTime.getTime() + playerRef.current.duration * 1000);
        onSeek(seekTime);
      }
    };

    video.addEventListener("canplay", handleCanPlay);
    video.addEventListener("waiting", handleWaiting);
    video.addEventListener("playing", handlePlaying);
    video.addEventListener("pause", handlePause);
    video.addEventListener("timeupdate", handleTimeUpdate);
    video.addEventListener("ended", handleEnded);

    return () => {
      video.removeEventListener("canplay", handleCanPlay);
      video.removeEventListener("waiting", handleWaiting);
      video.removeEventListener("playing", handlePlaying);
      video.removeEventListener("pause", handlePause);
      video.removeEventListener("timeupdate", handleTimeUpdate);
      video.removeEventListener("ended", handleEnded);
    };
  }, [onSeek, streamStartTime, isLive, currentVideoTime]);

  // Handle volume changes
  useEffect(() => {
    if (playerRef.current) {
      playerRef.current.volume = volume;
      playerRef.current.muted = isMuted;
    }
  }, [volume, isMuted]);

  const toggleMute = useCallback(() => {
    setIsMuted((prev) => !prev);
  }, []);

  const handleVolumeChange = useCallback(
    (newVolume: number) => {
      setVolume(newVolume);
      if (isMuted && newVolume > 0) setIsMuted(false);
    },
    [isMuted],
  );

  const handleFullscreen = useCallback(() => {
    // Check if it's iOS
    const isIOS = /iPad|iPhone|iPod/.test(navigator.userAgent);

    if (isIOS && videoRef.current) {
      // iOS: Use video element's webkit fullscreen API
      const video = videoRef.current as any;
      if (video.webkitEnterFullscreen) {
        video.webkitEnterFullscreen();
      } else if (video.webkitRequestFullscreen) {
        video.webkitRequestFullscreen();
      }
    } else if (onFullscreenToggle) {
      // Desktop/Android: Use container fullscreen
      onFullscreenToggle();
    }
  }, [onFullscreenToggle]);

  const handleUserClick = () => {
    if (needsUserInteraction && playerRef.current) {
      setNeedsUserInteraction(false);
      setIsPlaying(true);
      playerRef.current.play()?.catch((err: Error) => {
        console.error("Play error after user interaction:", err);
        setError(`${t("failedToPlay")}: ${err.message}`);
        onError?.(`${t("failedToPlay")}: ${err.message}`);
      });
    }
  };

  if (!channel) {
    return (
      <div className="flex h-full items-center justify-center bg-black text-white">{t("selectChannelToWatch")}</div>
    );
  }

  return (
    <div className="relative w-full bg-black md:h-full" onMouseMove={resetHideTimer} onMouseLeave={handleMouseLeave}>
      {/* Mobile: 16:9 aspect ratio container, Desktop: full height */}
      <div className="relative w-full aspect-video md:aspect-auto md:h-full">
        <video
          ref={videoRef}
          className="absolute inset-0 w-full h-full md:relative"
          playsInline
          webkit-playsinline="true"
          x5-playsinline="true"
          onClick={handleVideoClick}
        />

        {showLoading && (
          <div className="absolute top-4 left-4 md:top-8 md:left-8 flex items-center gap-2 md:gap-3 rounded-lg bg-white/10 ring-1 ring-white/20 backdrop-blur-sm px-3 py-2 md:px-4 md:py-3">
            <div className="relative h-4 w-4 md:h-5 md:w-5">
              <div className="absolute inset-0 rounded-full border-2 border-white/30" />
              <div className="absolute inset-0 rounded-full border-2 border-white border-t-transparent animate-spin" />
            </div>
            <span className="text-xs md:text-sm text-white font-medium">{t("loadingVideo")}</span>
          </div>
        )}

        {/* Channel Info and Controls */}
        <div
          className={`absolute top-4 right-4 md:top-8 md:right-8 flex flex-col gap-2 md:gap-3 items-end transition-opacity duration-300 ${
            showControls ? "opacity-100" : "opacity-0"
          }`}
        >
          <div className="flex flex-col gap-1.5 md:gap-2 px-2 py-1.5 md:px-3 md:py-2 items-center justify-center overflow-hidden rounded-lg bg-white/10 ring-1 ring-white/20 backdrop-blur-sm max-w-[calc(100vw-2rem)] md:max-w-none">
            {/* Logo Banner */}
            {channel.logo && (
              <img
                src={channel.logo}
                alt={channel.name}
                className="h-8 w-20 md:h-14 md:w-36 object-contain"
                onError={(e) => {
                  (e.target as HTMLImageElement).style.display = "none";
                }}
              />
            )}
            {/* Bottom Row: Channel Info */}
            <div className="flex items-center justify-center w-full">
              <div className="flex items-center gap-1.5 md:gap-2 min-w-0">
                <span className="rounded bg-white/10 px-1 py-0.5 md:px-1.5 text-[10px] md:text-xs font-medium text-white/60 shrink-0">
                  {channel.id}
                </span>
                <h2 className="text-xs md:text-base font-bold text-white truncate">{channel.name}</h2>
                <span className="text-xs md:text-sm text-white/50 hidden sm:inline">·</span>
                <div className="text-xs md:text-sm text-white/70 truncate hidden sm:block">{channel.group}</div>
              </div>
            </div>
          </div>
        </div>

        {needsUserInteraction && (
          <div
            className="absolute inset-0 flex cursor-pointer items-center justify-center bg-black/80 p-4 transition-opacity hover:bg-black/85"
            onClick={handleUserClick}
          >
            <div className="flex flex-col items-center gap-4 text-white">
              <svg className="h-20 w-20 opacity-90" fill="currentColor" viewBox="0 0 24 24">
                <path d="M8 5v14l11-7z" />
              </svg>
              <div className="text-center">
                <div className="mb-2 text-2xl font-semibold">{t("clickToPlay")}</div>
                <div className="text-sm text-white/70">{t("autoplayBlocked")}</div>
              </div>
            </div>
          </div>
        )}

        {error && (
          <div className="absolute inset-0 flex items-center justify-center bg-black/90 p-4">
            <div className="max-w-md rounded-lg bg-red-500/20 p-4 text-white">
              <div className="mb-2 text-lg font-semibold">{t("playbackError")}</div>
              <div className="text-sm">{error}</div>
            </div>
          </div>
        )}

        {/* Player Controls */}
        {channel && !error && !needsUserInteraction && (
          <div
            className={`absolute bottom-0 left-0 right-0 transition-opacity duration-300 ${
              showControls ? "opacity-100" : "opacity-0"
            }`}
            onMouseEnter={resetHideTimer}
          >
            <PlayerControls
              channel={channel}
              currentTime={currentVideoTime}
              currentProgram={currentProgram}
              isLive={isLive}
              onSeek={handleSeek}
              locale={locale}
              seekStartTime={streamStartTime}
              isPlaying={isPlaying}
              onPlayPause={togglePlayPause}
              volume={volume}
              onVolumeChange={handleVolumeChange}
              isMuted={isMuted}
              onMuteToggle={toggleMute}
              onFullscreen={handleFullscreen}
              showSidebar={showSidebar}
              onToggleSidebar={onToggleSidebar}
            />
          </div>
        )}
      </div>
    </div>
  );
}
