(ns editor.protobuf-types
  (:require [editor.protobuf :as protobuf]
            [editor.protobuf-forms :as protobuf-forms]
            [dynamo.graph :as g]
            [editor.geom :as geom]
            [editor.gl :as gl]
            [editor.gl.shader :as shader]
            [editor.gl.vertex :as vtx]
            [editor.defold-project :as project]
            [editor.scene :as scene]
            [editor.workspace :as workspace]
            [editor.resource :as resource]
            [editor.math :as math]
            [editor.gl.pass :as pass])
  (:import [com.dynamo.input.proto Input$InputBinding]
           [com.dynamo.render.proto Render$RenderPrototypeDesc]
           [com.dynamo.graphics.proto Graphics$TextureProfiles]
           [com.dynamo.gamesystem.proto GameSystem$LightDesc]
           [com.dynamo.physics.proto Physics$CollisionObjectDesc Physics$ConvexShape]
           [com.dynamo.input.proto Input$GamepadMaps]
           [com.dynamo.sound.proto Sound$SoundDesc]
           [com.jogamp.opengl.util.awt TextRenderer]
           [editor.types Region Animation Camera Image TexturePacking Rect EngineFormatTexture AABB TextureSetAnimationFrame TextureSetAnimation TextureSet]
           [java.awt.image BufferedImage]
           [java.io PushbackReader]
           [com.jogamp.opengl GL GL2 GLContext GLDrawableFactory]
           [com.jogamp.opengl.glu GLU]
           [javax.vecmath Matrix4d Point3d Quat4d]))

(set! *warn-on-reflection* true)

(def pb-defs [{:ext "input_binding"
               :icon "icons/32/Icons_35-Inputbinding.png"
               :pb-class Input$InputBinding
               :label "Input Binding"
               :view-types [:form-view :text]}
              {:ext "render"
               :icon "icons/32/Icons_30-Render.png"
               :pb-class Render$RenderPrototypeDesc
               :resource-fields [:script [:materials :material]]
               :view-types [:form-view :text]
               :label "Render"}
              {:ext "light"
               :label "Light"
               :icon "icons/32/Icons_21-Light.png"
               :pb-class GameSystem$LightDesc
               :tags #{:component}}
              {:ext "gamepads"
               :label "Gamepads"
               :icon "icons/32/Icons_34-Gamepad.png"
               :pb-class Input$GamepadMaps
               :view-types [:form-view :text]}
              {:ext "convexshape"
               :label "Convex Shape"
               ; TODO - missing icon
               :icon "icons/32/Icons_43-Tilesource-Collgroup.png"
               :pb-class Physics$ConvexShape}
              {:ext "sound"
               :label "Sound"
               :icon "icons/32/Icons_26-AT-Sound.png"
               :pb-class Sound$SoundDesc
               :resource-fields [:sound]
               :tags #{:component}}
              {:ext "texture_profiles"
               :label "Texture Profiles"
               :view-types [:form-view :text]
               :pb-class Graphics$TextureProfiles
               }])

(g/defnk produce-save-data [resource def pb]
  {:resource resource
   :textual? true
   :content (protobuf/map->str (:pb-class def) pb)})

(defn- build-pb [self basis resource dep-resources user-data]
  (let [def (:def user-data)
        pb  (:pb user-data)
        pb  (if (:transform-fn def) ((:transform-fn def) pb) pb)
        pb  (reduce (fn [pb [label resource]]
                      (if (vector? label)
                        (assoc-in pb label resource)
                        (assoc pb label resource)))
                    pb (map (fn [[label res]]
                              [label (resource/proj-path (get dep-resources res))])
                            (:dep-resources user-data)))]
    {:resource resource :content (protobuf/map->bytes (:pb-class user-data) pb)}))

(g/defnk produce-build-targets [_node-id resource pb def dep-build-targets]
  (let [dep-build-targets (flatten dep-build-targets)
        deps-by-source (into {} (map #(let [res (:resource %)] [(resource/proj-path (:resource res)) res]) dep-build-targets))
        resource-fields (mapcat (fn [field] (if (vector? field) (mapv (fn [i] (into [(first field) i] (rest field))) (range (count (get pb (first field))))) [field])) (:resource-fields def))
        dep-resources (map (fn [label] [label (get deps-by-source (if (vector? label) (get-in pb label) (get pb label)))]) resource-fields)]
    [{:node-id _node-id
      :resource (workspace/make-build-resource resource)
      :build-fn build-pb
      :user-data {:pb pb
                  :pb-class (:pb-class def)
                  :def def
                  :dep-resources dep-resources}
      :deps dep-build-targets}]))

(g/defnk produce-form-data [_node-id pb def]
  (protobuf-forms/produce-form-data _node-id pb def))

(g/defnode ProtobufNode
  (inherits project/ResourceNode)

  (property pb g/Any (dynamic visible (g/constantly false)))
  (property def g/Any (dynamic visible (g/constantly false)))

  (output form-data g/Any :cached produce-form-data)

  (input dep-build-targets g/Any :array)

  (output save-data g/Any :cached produce-save-data)
  (output build-targets g/Any :cached produce-build-targets)
  (output scene g/Any (g/constantly {})))

(defn- connect-build-targets [project self resource path]
  (let [resource (workspace/resolve-resource resource path)]
    (project/connect-resource-node project resource self [[:build-targets :dep-build-targets]])))

(defn load-pb [project self resource def]
  (let [pb (protobuf/read-text (:pb-class def) resource)]
    (concat
     (g/set-property self :pb pb)
     (g/set-property self :def def)
     (for [res (:resource-fields def)]
       (if (vector? res)
         (for [v (get pb (first res))]
           (let [path (if (second res) (get v (second res)) v)]
             (connect-build-targets project self resource path)))
         (connect-build-targets project self resource (get pb res)))))))

(defn- register [workspace def]
  (let [ext (:ext def)
        exts (if (vector? ext) ext [ext])]
    (for [ext exts]
      (workspace/register-resource-type workspace
                                     :ext ext
                                     :label (:label def)
                                     :build-ext (:build-ext def)
                                     :node-type ProtobufNode
                                     :load-fn (fn [project self resource] (load-pb project self resource def))
                                     :icon (:icon def)
                                     :view-types (:view-types def)
                                     :tags (:tags def)
                                     :template (:template def)))))

(defn register-resource-types [workspace]
  (for [def pb-defs]
    (register workspace def)))
