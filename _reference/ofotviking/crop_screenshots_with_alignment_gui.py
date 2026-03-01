import argparse
from enum import Enum
import os
import sys
import glob
from PIL import Image, ImageOps, ImageFilter
import numpy as np
from scipy import fftpack
import tkinter as tk
from tkinter import ttk, Canvas, Scale, IntVar, StringVar
from PIL import ImageTk

# Configuration - Make sure this matches the Enfusion Workbench tool settings
TILE_CROP_SIZE = 550 # pixels - Set this initially to be too large for perfect tiling
TILE_OVERLAP = -7  # pixels - Then adjust this value to get the perfect tiling testing in -make_map mode

# Optional configuration
SKIP_EXISTING_TILES = True # Skip creating tiles that already exist
DELETE_ORIGINALS = False # Delete the original screenshots after cropping the tiles to save disk space

INTERMEDIATE_TILE_FILENAME_SUFFIX = "tile" # only change this if you have also changed the Enfusion Workbench settings
FINAL_TILE_FILENAME = "tile"
FINAL_TILE_IMAGE_TYPE = "jpg"


class Screenshot():
    xCoordWS: int
    zCoordWS: int
    # This has two images, the full resolution raw screenshot, and the cropped tile
    _screenshot_filepath: str|None

    _tile_filepath: str
    _screenshot_image: Image
    _tile_image: Image

    def __init__(self, xCoordWS: int, zCoordWS: int, screenshot_filepath: str|None = None, tile_filepath: str|None = None):
        self.xCoordWS = xCoordWS
        self.zCoordWS = zCoordWS
        self._screenshot_filepath = screenshot_filepath
        self._tile_filepath = tile_filepath
        self._screenshot_image = None
        self._tile_image = None

    def __str__(self):
        return f"Screenshot {self.xCoordWS}x{self.zCoordWS}, screenshot_filepath={self.screenshot_filepath}, tile_filepath={self.tile_filepath}"
    
    @property
    def screenshot_image(self):
        if self._screenshot_image is None:
            if os.path.exists(self.screenshot_filepath):            
                self._screenshot_image = Image.open(self.screenshot_filepath)
            else:
                raise RuntimeError(f"Screenshot image not found at {self.screenshot_filepath}")
        return self._screenshot_image

    @property
    def tile_image(self) -> Image.Image:
        if self._tile_image is None:
            self._tile_image = Image.open(self.tile_filepath)
        return self._tile_image

    @property
    def screenshot_filepath(self):
        return self._screenshot_filepath

    @property
    def tile_filepath(self):
        if self._tile_filepath is not None:
            return self._tile_filepath
        return self.generate_tile_path()

    def unload(self):
        self._screenshot_image = None
        self._tile_image = None

    def generate_tile_path(self):
        # take filepath, strip of .png, and add output_file_suffix + output_tile_type
        return self.screenshot_filepath.replace(".png", f"_{INTERMEDIATE_TILE_FILENAME_SUFFIX}.png")
    
    @property
    def coordinate_string(self) -> str:
        return self.make_coordinate_string(self.xCoordWS, self.zCoordWS)
    
    @classmethod
    def make_coordinate_string(cls, x: int, z: int) -> str:
        return f"Screenshot_{x}_{z}"

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, Screenshot):
            return False
        return self.xCoordWS == other.xCoordWS and self.zCoordWS == other.zCoordWS

    def __hash__(self) -> int:
        return hash(self.coordinate_string)
    
    def create_tile(self):
        # crop the center of the image to crop_size x crop_size
        width, height = self.screenshot_image.size
        left = (width - TILE_CROP_SIZE) / 2
        top = (height - TILE_CROP_SIZE) / 2
        right = (width + TILE_CROP_SIZE) / 2
        bottom = (height + TILE_CROP_SIZE) / 2
        cropped_image = self.screenshot_image.crop((left, top, right, bottom))
        # set the jpeg quality to 95
        cropped_image.save(self.tile_filepath, quality=95)

    def tile_exists(self):
        return os.path.exists(self.tile_filepath)
    
    def get_unit_coordinates(self, min_x: int, min_z: int, filename_coordinate_step: int):
        return (int((self.xCoordWS - min_x) / filename_coordinate_step), int((self.zCoordWS - min_z) / filename_coordinate_step))


class TileAlignmentGUI:
    """GUI for aligning screenshot tiles and finding the optimal overlap value."""
    
    def __init__(self, root, processor):
        self.root = root
        self.processor = processor
        self.root.title("Tile Alignment Tool")
        
        # Find adjacent tiles to work with
        self.selected_tile_index = 0
        self.tiles = self.processor.screenshots
        if len(self.tiles) < 2:
            raise RuntimeError("Need at least 2 screenshots to align")
            
        # Find all valid tile sets (tiles with neighbors)
        self.valid_tile_sets = self.find_valid_tile_sets()
        if not self.valid_tile_sets:
            raise RuntimeError("Could not find any adjacent tiles to align")
        
        # Current set index
        self.current_set_index = 0
        self.load_current_tile_set()
            
        # Initialize variables
        self.overlap_var = IntVar(value=abs(TILE_OVERLAP))
        self.crop_size_var = IntVar(value=TILE_CROP_SIZE)
        self.direction_var = StringVar(value="Horizontal" if self.horizontal_neighbor else "Vertical")
        
        # Keep track of original values for reset functionality
        self.original_overlap = TILE_OVERLAP
        self.original_crop_size = TILE_CROP_SIZE
        
        # Setup the UI
        self.setup_ui()
        
        # Initial render
        self.update_preview()
    
    def find_valid_tile_sets(self):
        """Find all tiles that have at least one neighbor."""
        valid_sets = []
        
        for tile in self.tiles:
            h_neighbor = self.processor.find_neighbour(tile, self.processor.tile_step_size, 0)
            v_neighbor = self.processor.find_neighbour(tile, 0, self.processor.tile_step_size)
            
            # If this tile has at least one neighbor, it's a valid set
            if h_neighbor or v_neighbor:
                valid_sets.append({
                    'tile': tile,
                    'h_neighbor': h_neighbor,
                    'v_neighbor': v_neighbor
                })
        
        return valid_sets
    
    def load_current_tile_set(self):
        """Load the current tile set based on the current_set_index."""
        current_set = self.valid_tile_sets[self.current_set_index]
        self.current_tile = current_set['tile']
        self.horizontal_neighbor = current_set['h_neighbor']
        self.vertical_neighbor = current_set['v_neighbor']
        
        # Update selected tile index for the dropdown selector
        for i, tile in enumerate(self.tiles):
            if tile == self.current_tile:
                self.selected_tile_index = i
                break
    
    def next_set(self):
        """Move to the next valid tile set."""
        if len(self.valid_tile_sets) > 1:
            self.current_set_index = (self.current_set_index + 1) % len(self.valid_tile_sets)
            self.load_current_tile_set()
            self.update_ui_for_new_set()
            self.update_preview()
            self.info_var.set(self.get_info_text())
    
    def prev_set(self):
        """Move to the previous valid tile set."""
        if len(self.valid_tile_sets) > 1:
            self.current_set_index = (self.current_set_index - 1) % len(self.valid_tile_sets)
            self.load_current_tile_set()
            self.update_ui_for_new_set()
            self.update_preview()
            self.info_var.set(self.get_info_text())
    
    def update_ui_for_new_set(self):
        """Update UI elements when switching to a new tile set."""
        # Update the tile selector dropdown
        self.tile_selector.current(self.selected_tile_index)
        
        # Update direction dropdown values
        direction_options = ["Horizontal", "Vertical"] if self.horizontal_neighbor and self.vertical_neighbor else \
                          ["Horizontal"] if self.horizontal_neighbor else ["Vertical"]
        
        self.direction_dropdown['values'] = direction_options
        
        # Check if current direction is valid for new set
        current_direction = self.direction_var.get()
        if current_direction == "Horizontal" and not self.horizontal_neighbor:
            self.direction_var.set("Vertical")
        elif current_direction == "Vertical" and not self.vertical_neighbor:
            self.direction_var.set("Horizontal")
    
    def setup_ui(self):
        # Main frame
        main_frame = ttk.Frame(self.root, padding="10")
        main_frame.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))
        
        # Preview canvas
        self.canvas = Canvas(main_frame, width=1200, height=600, bg="black")
        self.canvas.grid(row=0, column=0, columnspan=2, sticky=(tk.W, tk.E, tk.N, tk.S))
        
        # Navigation frame - add this above the controls
        nav_frame = ttk.Frame(main_frame, padding="5")
        nav_frame.grid(row=1, column=0, columnspan=2, sticky=(tk.W, tk.E))
        
        # Navigation buttons
        ttk.Button(nav_frame, text="← Previous Set", command=self.prev_set).grid(row=0, column=0, padx=(0, 10))
        
        # Add set indicator label
        self.set_indicator = StringVar(value=f"Set {self.current_set_index + 1} of {len(self.valid_tile_sets)}")
        ttk.Label(nav_frame, textvariable=self.set_indicator, width=20, anchor="center").grid(row=0, column=1)
        
        ttk.Button(nav_frame, text="Next Set →", command=self.next_set).grid(row=0, column=2, padx=(10, 0))
        
        # Configure navigation frame columns
        nav_frame.columnconfigure(0, weight=1)
        nav_frame.columnconfigure(1, weight=2)
        nav_frame.columnconfigure(2, weight=1)
        
        # Controls frame
        controls_frame = ttk.Frame(main_frame, padding="5")
        controls_frame.grid(row=2, column=0, columnspan=2, sticky=(tk.W, tk.E))
        
        # Overlap slider 
        ttk.Label(controls_frame, text="Overlap:").grid(row=0, column=0, sticky=tk.W)
        overlap_slider = Scale(controls_frame, from_=0, to=200, orient=tk.HORIZONTAL, 
                              variable=self.overlap_var, command=self.on_overlap_change,
                              length=300)
        overlap_slider.grid(row=0, column=1, sticky=(tk.W, tk.E))
        
        # Add numeric entry field for precise overlap value
        ttk.Label(controls_frame, text="Value:").grid(row=0, column=2, sticky=tk.W, padx=(5, 0))
        self.overlap_entry = ttk.Entry(controls_frame, width=5, justify='right')
        self.overlap_entry.insert(0, str(self.overlap_var.get()))
        self.overlap_entry.grid(row=0, column=3, sticky=tk.W, padx=(2, 10))
        self.overlap_entry.bind("<Return>", self.on_overlap_entry_change)
        
        # Add crop size slider (new control)
        ttk.Label(controls_frame, text="Crop Size:").grid(row=1, column=0, sticky=tk.W)
        crop_size_slider = Scale(controls_frame, from_=300, to=1500, orient=tk.HORIZONTAL,
                               variable=self.crop_size_var, command=self.on_crop_size_change,
                               length=300)
        crop_size_slider.grid(row=1, column=1, sticky=(tk.W, tk.E))
        
        # Add numeric entry field for precise crop size value
        ttk.Label(controls_frame, text="Value:").grid(row=1, column=2, sticky=tk.W, padx=(5, 0))
        self.crop_size_entry = ttk.Entry(controls_frame, width=5, justify='right')
        self.crop_size_entry.insert(0, str(self.crop_size_var.get()))
        self.crop_size_entry.grid(row=1, column=3, sticky=tk.W, padx=(2, 10))
        self.crop_size_entry.bind("<Return>", self.on_crop_size_entry_change)
        
        # Direction selector
        ttk.Label(controls_frame, text="Direction:").grid(row=0, column=4, sticky=tk.W, padx=(20, 0))
        direction_options = ["Horizontal", "Vertical"] if self.horizontal_neighbor and self.vertical_neighbor else \
                            ["Horizontal"] if self.horizontal_neighbor else ["Vertical"]
        
        self.direction_dropdown = ttk.Combobox(controls_frame, textvariable=self.direction_var, 
                                        values=direction_options, state="readonly", width=10)
        self.direction_dropdown.grid(row=0, column=5, sticky=tk.W)
        self.direction_dropdown.bind("<<ComboboxSelected>>", self.on_direction_change)
        
        # Tile selector
        ttk.Label(controls_frame, text="Tile:").grid(row=1, column=4, sticky=tk.W, padx=(20, 0))
        self.tile_selector = ttk.Combobox(controls_frame, 
                                     values=[f"{t.xCoordWS}x{t.zCoordWS}" for t in self.tiles], 
                                     state="readonly", width=15)
        self.tile_selector.current(self.selected_tile_index)
        self.tile_selector.grid(row=1, column=5, sticky=tk.W)
        self.tile_selector.bind("<<ComboboxSelected>>", self.on_tile_change)
        
        # Button frame
        button_frame = ttk.Frame(main_frame, padding="5")
        button_frame.grid(row=3, column=0, columnspan=2, sticky=(tk.W, tk.E))
        
        # Apply button
        ttk.Button(button_frame, text="Apply Settings", command=self.apply_settings).grid(row=0, column=0, padx=(0, 10))
        
        # Generate Tiles button
        ttk.Button(button_frame, text="Generate Tiles", command=self.generate_tiles).grid(row=0, column=1, padx=(10, 0))
        
        # Reset button
        ttk.Button(button_frame, text="Reset to Original", command=self.reset_to_original).grid(row=0, column=2, padx=(10, 0))
        
        # Configure button frame columns
        button_frame.columnconfigure(0, weight=1)
        button_frame.columnconfigure(1, weight=1)
        button_frame.columnconfigure(2, weight=1)
        
        # Info label
        self.info_var = StringVar(value=self.get_info_text())
        info_label = ttk.Label(main_frame, textvariable=self.info_var)
        info_label.grid(row=4, column=0, columnspan=2, sticky=tk.W, pady=(5, 0))
        
        # Configure grid weights
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)
        main_frame.columnconfigure(0, weight=1)
        main_frame.rowconfigure(0, weight=1)
        controls_frame.columnconfigure(1, weight=1)
        
    def get_neighbor_for_current_direction(self):
        if self.direction_var.get() == "Horizontal":
            return self.horizontal_neighbor
        else:
            return self.vertical_neighbor
            
    def get_info_text(self):
        overlap = self.overlap_var.get()
        crop_size = self.crop_size_var.get()
        direction = self.direction_var.get()
        neighbor = self.get_neighbor_for_current_direction()
        
        # Update set indicator
        self.set_indicator.set(f"Set {self.current_set_index + 1} of {len(self.valid_tile_sets)}")
        
        return (f"Current tile: {self.current_tile.xCoordWS}x{self.current_tile.zCoordWS}, "
                f"{direction} neighbor: {neighbor.xCoordWS}x{neighbor.zCoordWS}, "
                f"Overlap: {overlap} pixels, Crop Size: {crop_size} pixels")
        
    def on_overlap_change(self, *args):
        # Update the entry field to match the slider
        self.overlap_entry.delete(0, tk.END)
        self.overlap_entry.insert(0, str(self.overlap_var.get()))
        
        self.update_preview()
        self.info_var.set(self.get_info_text())
        
    def on_overlap_entry_change(self, event):
        try:
            new_value = int(self.overlap_entry.get())
            # Ensure the value is within the slider range
            if new_value < 0:
                new_value = 0
            elif new_value > 200:
                new_value = 200
                
            self.overlap_var.set(new_value)
            self.update_preview()
            self.info_var.set(self.get_info_text())
        except ValueError:
            self.info_var.set("Invalid overlap value. Please enter a number.")
            # Reset entry to current slider value
            self.overlap_entry.delete(0, tk.END)
            self.overlap_entry.insert(0, str(self.overlap_var.get()))
    
    def on_crop_size_change(self, *args):
        # Update the entry field to match the slider
        self.crop_size_entry.delete(0, tk.END)
        self.crop_size_entry.insert(0, str(self.crop_size_var.get()))
        
        self.update_preview()
        self.info_var.set(self.get_info_text())
        
    def on_crop_size_entry_change(self, event):
        try:
            new_value = int(self.crop_size_entry.get())
            # Ensure the value is within a reasonable range
            if new_value < 300:
                new_value = 300
            elif new_value > 1500:
                new_value = 1500
                
            self.crop_size_var.set(new_value)
            self.update_preview()
            self.info_var.set(self.get_info_text())
        except ValueError:
            self.info_var.set("Invalid crop size value. Please enter a number.")
            # Reset entry to current slider value
            self.crop_size_entry.delete(0, tk.END)
            self.crop_size_entry.insert(0, str(self.crop_size_var.get()))
        
    def on_direction_change(self, *args):
        self.update_preview()
        self.info_var.set(self.get_info_text())
        
    def on_tile_change(self, *args):
        idx = self.tile_selector.current()
        self.selected_tile_index = idx
        self.current_tile = self.tiles[idx]
        
        # Find neighbors for the new current tile
        self.horizontal_neighbor = self.processor.find_neighbour(
            self.current_tile, self.processor.tile_step_size, 0)
        self.vertical_neighbor = self.processor.find_neighbour(
            self.current_tile, 0, self.processor.tile_step_size)
        
        # Find matching set in valid_tile_sets
        for i, tile_set in enumerate(self.valid_tile_sets):
            if tile_set['tile'] == self.current_tile:
                self.current_set_index = i
                break
        
        # If the current direction is no longer valid, switch it
        if self.direction_var.get() == "Horizontal" and not self.horizontal_neighbor:
            if self.vertical_neighbor:
                self.direction_var.set("Vertical")
            else:
                # No neighbors available, find a different tile
                self.info_var.set("No neighbors available for this tile. Please select another.")
                return
        elif self.direction_var.get() == "Vertical" and not self.vertical_neighbor:
            if self.horizontal_neighbor:
                self.direction_var.set("Horizontal")
            else:
                # No neighbors available, find a different tile
                self.info_var.set("No neighbors available for this tile. Please select another.")
                return
        
        self.update_preview()
        self.info_var.set(self.get_info_text())
    
    def get_image_for_tile(self, tile):
        """Try to get either tile_image or screenshot_image, with appropriate fallback handling."""
        try:
            # First try to use the tile image if it exists
            if os.path.exists(tile.tile_filepath):
                return tile.tile_image.copy()
            # Fall back to the screenshot image if tile doesn't exist
            elif tile.screenshot_filepath and os.path.exists(tile.screenshot_filepath):
                # If we're using screenshot images, we need to crop them to the active crop size
                img = tile.screenshot_image.copy()
                width, height = img.size
                crop_size = self.crop_size_var.get()
                left = (width - crop_size) / 2
                top = (height - crop_size) / 2
                right = (width + crop_size) / 2
                bottom = (height + crop_size) / 2
                return img.crop((left, top, right, bottom))
            else:
                raise RuntimeError(f"No image found for tile at {tile.xCoordWS}x{tile.zCoordWS}. "
                                  f"Tried {tile.tile_filepath} and {tile.screenshot_filepath}")
        except Exception as e:
            print(f"Error loading image for {tile.xCoordWS}x{tile.zCoordWS}: {e}")
            # Create a blank image as a last resort
            return Image.new("RGB", (self.crop_size_var.get(), self.crop_size_var.get()), "gray")
        
    def update_preview(self):
        self.canvas.delete("all")
        
        # Get the current settings
        overlap = self.overlap_var.get()
        crop_size = self.crop_size_var.get()
        
        try:
            # Load current tile image
            current_img = self.get_image_for_tile(self.current_tile)
            img_size = current_img.size[0]  # Assuming square tiles
            
            neighbor = self.get_neighbor_for_current_direction()
            if not neighbor:
                self.canvas.create_text(
                    600, 300, text="No neighbor available in this direction", fill="white", font=("Arial", 16))
                return
            
            # Load neighbor tile image
            neighbor_img = self.get_image_for_tile(neighbor)
            
            # Create a composite image based on the direction
            if self.direction_var.get() == "Horizontal":
                # Horizontal alignment (neighbor on the right)
                composite_width = img_size * 2 - overlap
                composite_height = img_size
                composite = Image.new("RGB", (composite_width, composite_height))
                composite.paste(current_img, (0, 0))
                composite.paste(neighbor_img, (img_size - overlap, 0))
            else:
                # Vertical alignment (neighbor below)
                composite_width = img_size
                composite_height = img_size * 2 - overlap
                composite = Image.new("RGB", (composite_width, composite_height))
                composite.paste(current_img, (0, 0))
                composite.paste(neighbor_img, (0, img_size - overlap))
            
            # Draw gridlines
            self.draw_gridlines(composite, img_size, overlap)
            
            # Resize if needed to fit the canvas
            canvas_width = 1200
            canvas_height = 600
            scale_factor = min(canvas_width / composite_width, canvas_height / composite_height)
            
            if scale_factor < 1:
                new_size = (int(composite_width * scale_factor), int(composite_height * scale_factor))
                composite = composite.resize(new_size, Image.LANCZOS)
            
            # Convert to PhotoImage and display
            self.photo = ImageTk.PhotoImage(composite)
            self.canvas.create_image(canvas_width // 2, canvas_height // 2, image=self.photo)
        except Exception as e:
            self.canvas.create_text(
                600, 300, text=f"Error creating preview: {str(e)}", fill="white", font=("Arial", 16))
            import traceback
            traceback.print_exc()
    
    def draw_gridlines(self, img, tile_size, overlap):
        """Draw gridlines on the composite image to show tile boundaries."""
        from PIL import ImageDraw  # Import here to avoid import errors if not using GUI
        draw = ImageDraw.Draw(img)
        
        # Draw a rectangle around the first tile
        draw.rectangle([0, 0, tile_size-1, tile_size-1], outline="yellow", width=2)
        
        # Draw a rectangle around the second tile
        if self.direction_var.get() == "Horizontal":
            draw.rectangle([tile_size-overlap, 0, 2*tile_size-overlap-1, tile_size-1], outline="cyan", width=2)
            # Draw overlap area
            draw.rectangle([tile_size-overlap, 0, tile_size-1, tile_size-1], outline="red", width=2)
        else:
            draw.rectangle([0, tile_size-overlap, tile_size-1, 2*tile_size-overlap-1], outline="cyan", width=2)
            # Draw overlap area
            draw.rectangle([0, tile_size-overlap, tile_size-1, tile_size-1], outline="red", width=2)
    
    def apply_settings(self):
        """Apply the selected settings globally."""
        new_overlap = -self.overlap_var.get()  # Negative for tile displacement
        new_crop_size = self.crop_size_var.get()
        
        global TILE_OVERLAP, TILE_CROP_SIZE
        TILE_OVERLAP = new_overlap
        TILE_CROP_SIZE = new_crop_size
        
        # Show confirmation
        print(f"Applied new settings: TILE_OVERLAP = {TILE_OVERLAP}, TILE_CROP_SIZE = {TILE_CROP_SIZE}")
        self.info_var.set(f"Applied settings: TILE_OVERLAP = {TILE_OVERLAP}, TILE_CROP_SIZE = {TILE_CROP_SIZE}. " + 
                        self.get_info_text())
    
    def reset_to_original(self):
        """Reset to the original values."""
        self.overlap_var.set(abs(self.original_overlap))
        self.crop_size_var.set(self.original_crop_size)
        
        # Update entry fields
        self.overlap_entry.delete(0, tk.END)
        self.overlap_entry.insert(0, str(self.overlap_var.get()))
        
        self.crop_size_entry.delete(0, tk.END)
        self.crop_size_entry.insert(0, str(self.crop_size_var.get()))
        
        self.update_preview()
        self.info_var.set("Reset to original values. " + self.get_info_text())
    
    def generate_tiles(self):
        """Generate tiles from screenshots for the selected tiles using current settings."""
        try:
            # Apply the current settings first
            self.apply_settings()
            
            # Generate tiles for current tile and neighbor
            self.current_tile.create_tile()
            neighbor = self.get_neighbor_for_current_direction()
            if neighbor:
                neighbor.create_tile()
            
            # Update the preview with the newly generated tiles
            self.update_preview()
            self.info_var.set("Generated tiles successfully with current settings. " + self.get_info_text())
        except Exception as e:
            self.info_var.set(f"Error generating tiles: {str(e)}")
            import traceback
            traceback.print_exc()


class ScreenshotProcessor():
    screenshots: list[Screenshot]
    mapped_screenshots: dict[str, Screenshot]

    def __init__(self, screenshots: list[Screenshot]|None = None):
        if screenshots is None:
            screenshots = []
        self.screenshots = screenshots
        self.mapped_screenshots = {}
        for screenshot in screenshots:
            self.mapped_screenshots[screenshot.coordinate_string] = screenshot
        if len(screenshots) != len(self.mapped_screenshots):
            raise RuntimeError("WARNING: Duplicate screenshots found")

        if len(screenshots) > 0:
            self.sort()
    
    @classmethod
    def from_directory(cls, directory: str):
        screenshot_processor = ScreenshotProcessor()
        
        glob_match = os.path.join(directory, f"*/*.png")
        matching_filepaths = glob.glob(glob_match)
        if len(matching_filepaths) == 0:
            raise print(f"ERROR: No screenshots found in {directory}")

        print(f"Importing {len(matching_filepaths)} files")
        for filepath in matching_filepaths:
            # incoming screenshot filenames are in the format
            # {prefix}_{x}_{z}.png - The original full resolution screenshot
            # {prefix}_{x}_{z}_tile.png - The cropped tile
            filename = os.path.basename(filepath)
            filename_no_ext = os.path.splitext(filename)[0]
            filename_elements = filename_no_ext.split("_")
            if filename_elements[-1] == INTERMEDIATE_TILE_FILENAME_SUFFIX:
                x = int(filename_elements[-3])
                z = int(filename_elements[-2])
                screenshot_processor.add_screenshot(Screenshot(x, z, tile_filepath=filepath))
            else:
                x = int(filename_elements[-2])
                z = int(filename_elements[-1])
                screenshot_processor.add_screenshot(Screenshot(x, z, screenshot_filepath=filepath))

        return screenshot_processor
        
    def __str__(self):
        return f"ScreenshotProcessor {len(self.screenshots)} screenshots"
    
    def __repr__(self):
        return str(self)


    def add_screenshot(self, screenshot: Screenshot):
        if screenshot in self.mapped_screenshots:
            return
        self.screenshots.append(screenshot)
        self.mapped_screenshots[screenshot.coordinate_string] = screenshot
        self.sort()

    def sort(self):
        self.screenshots = sorted(self.screenshots, key=lambda screenshot: (screenshot.xCoordWS, screenshot.zCoordWS))
    
    def min_x(self):
        # scan the list of screenshots and find the minimum x coordinate
        return min([screenshot.xCoordWS for screenshot in self.screenshots
                    if screenshot.xCoordWS is not None])
    
    def max_x(self):
        return max([screenshot.xCoordWS for screenshot in self.screenshots
                    if screenshot.xCoordWS is not None])
    
    def min_z(self):
        return min([screenshot.zCoordWS for screenshot in self.screenshots
                    if screenshot.zCoordWS is not None])
    
    def max_z(self):
        return max([screenshot.zCoordWS for screenshot in self.screenshots
                    if screenshot.zCoordWS is not None])

    @property
    def tile_step_size(self):
        # take the first two tiles, and calculate the difference in x and z
        tile_0 = self.screenshots[0]
        tile_1 = self.screenshots[1]
        x_diff = abs(tile_0.xCoordWS - tile_1.xCoordWS)
        z_diff = abs(tile_0.zCoordWS - tile_1.zCoordWS)
        # return which ever is larger
        return max(x_diff, z_diff)

    def count(self):
        return len(self.screenshots)

    def make_tiles(self):
        for screenshot in self.screenshots:
            if screenshot.tile_exists() and SKIP_EXISTING_TILES:
                print(f"Skipping {screenshot.tile_filepath}")
            else:
                print(f"Creating cropped screenshot tile for coordinate {screenshot.xCoordWS}, {screenshot.zCoordWS}")
                screenshot.create_tile()

            if DELETE_ORIGINALS and screenshot.screenshot_filepath is not None:
                os.remove(screenshot.screenshot_filepath)

    def composite_screenshot_tiles(self, tiles: list[Screenshot], output_filename: str):
        tile_min_x = min([tile.xCoordWS for tile in tiles])
        tile_min_z = min([tile.zCoordWS for tile in tiles])
        tile_max_x = max([tile.xCoordWS for tile in tiles])
        tile_max_z = max([tile.zCoordWS for tile in tiles])

        # worldspace range
        x_ws_range = tile_max_x - tile_min_x
        z_ws_range = tile_max_z - tile_min_z

        # unit range
        x_unit_range = int(x_ws_range / self.tile_step_size) + 1
        z_unit_range = int(z_ws_range / self.tile_step_size) + 1

        output_image_size_x = x_unit_range * TILE_CROP_SIZE
        output_image_size_z = z_unit_range * TILE_CROP_SIZE

        # we need to account for the overlap by adding the <dim>_unit_range * overlap from the output size
        # don't forget that get_tile_overlap() returns a negative number
        if TILE_OVERLAP != 0:
            output_image_size_x += (x_unit_range - 1) * TILE_OVERLAP
            output_image_size_z += (z_unit_range - 1) * TILE_OVERLAP

        # create a new image with the size of the map
        map_image = Image.new("RGB", (output_image_size_x, output_image_size_z), (0, 0, 0, 0))
        sorted_tiles = sorted(tiles, key=lambda tile: (tile.xCoordWS, tile.zCoordWS))

        for tile in sorted_tiles:
            x, z = tile.get_unit_coordinates(tile_min_x, tile_min_z, self.tile_step_size)

            # flip the z coordinate so that the origin is at the bottom left
            z = z_unit_range - z - 1

            paste_tile_coord_x = int(x * TILE_CROP_SIZE)
            paste_tile_coord_z = int(z * TILE_CROP_SIZE)

            # OPTIONAL: now account for how much we want to overlap the tiles
            if TILE_OVERLAP != 0:
                displacement_x = int(TILE_OVERLAP * x)
                displacement_z = int(TILE_OVERLAP * z)
                print(f"Adjusting tile coordinates by overlap of {displacement_x},{displacement_z} px")
                paste_tile_coord_x += displacement_x
                paste_tile_coord_z += displacement_z

            print(f"Placing {tile.tile_filepath} at {paste_tile_coord_x}, {paste_tile_coord_z} (unit {x}, {z})")
            map_image.paste(tile.tile_image, (paste_tile_coord_x, paste_tile_coord_z))
            tile.unload()
        
        # save the map image
        map_image.save(output_filename, quality=96)
        print(f"Saved tiles to {output_filename}")


    def make_large_map(self, filepath: str = "map.jpeg", x_coods_start: int = -1, z_coord_start: int = -1, max_x_tile_count: int = -1, max_z_tile_count: int = -1):
        if x_coods_start < 0 and z_coord_start < 0 and max_x_tile_count < 0 and max_z_tile_count < 0:
            print("Creating large map from all tiles")
            self.composite_screenshot_tiles(self.screenshots, filepath)
            return

        included_tiles = []
        min_x_coord = -1
        min_z_coord = -1
        max_x_coord = -1
        max_z_coord = -1
        for screenshot in self.screenshots:
            if x_coods_start > 0 and screenshot.xCoordWS < x_coods_start:
                continue
            if z_coord_start > 0 and screenshot.zCoordWS < z_coord_start:
                continue
            
            # update our mins and maxes
            if min_x_coord == -1 or screenshot.xCoordWS < min_x_coord:
                min_x_coord = screenshot.xCoordWS
            if min_z_coord == -1 or screenshot.zCoordWS < min_z_coord:
                min_z_coord = screenshot.zCoordWS
            if max_x_coord == -1 or screenshot.xCoordWS > max_x_coord:
                max_x_coord = screenshot.xCoordWS
            if max_z_coord == -1 or screenshot.zCoordWS > max_z_coord:
                max_z_coord = screenshot.zCoordWS
            
            x_tile_count = int((max_x_coord - min_x_coord) / self.tile_step_size)
            z_tile_count = int((max_z_coord - min_z_coord) / self.tile_step_size)

            # print(f"min_x_coord: {min_x_coord}, min_z_coord: {min_z_coord}, max_x_coord: {max_x_coord}, max_z_coord: {max_z_coord}")
            # print(f"Current axis tile counts: {x_tile_count}, {z_tile_count}")

            if (max_x_tile_count == 0 or x_tile_count <= max_x_tile_count) or (max_z_tile_count == 0 or z_tile_count <= max_z_tile_count):
                included_tiles.append(screenshot)

        print(f"Creating large map from {len(included_tiles)} tiles (min_x: {min_x_coord}, min_z: {min_z_coord}, max_x: {max_x_coord}, max_z: {max_z_coord})")
        self.composite_screenshot_tiles(included_tiles, filepath)

    def make_initial_tiles(self, output_directory: str, initial_z_dirname: int):
        # Initial z should usually be 5, as we support 5 levels of detail
        for screenshot in self.screenshots:
            normalized_x = int(screenshot.xCoordWS / self.tile_step_size)
            normalized_z = int(screenshot.zCoordWS / self.tile_step_size)

            # Folder structure is output_directory/initial_z_dirname/normalized_x/normalized_z
            # i.e. output_directory/5/0/0/tile.jpg
            intial_tile_filepath = os.path.join(output_directory, str(initial_z_dirname), str(normalized_x), str(normalized_z), f"{FINAL_TILE_FILENAME}.{FINAL_TILE_IMAGE_TYPE}")
            # copy the tile to the new folder
            if not os.path.exists(intial_tile_filepath):
                tile_directory_path = os.path.dirname(intial_tile_filepath)
                os.makedirs(tile_directory_path, exist_ok=True)

                print(f"Converting {screenshot.tile_filepath} to {intial_tile_filepath}")
                image = Image.open(screenshot.tile_filepath)

                
                target_size = TILE_CROP_SIZE + TILE_OVERLAP
                image_size = image.size
                if image_size[0] != target_size or image_size[1] != target_size:
                    left = (image_size[0] - target_size) / 2
                    top = (image_size[1] - target_size) / 2
                    right = (image_size[0] + target_size) / 2
                    bottom = (image_size[1] + target_size) / 2
                    image = image.crop((left, top, right, bottom))

                image.save(intial_tile_filepath, quality=95)

    def auto_find_crop(self):
        # Find the highest detail screenshot
        
        # highest_detail_screenshot, highest_detail = self.find_highest_detail_screenshot()
        
        source_screenshot = self.mapped_screenshots[Screenshot.make_coordinate_string(5700, 3800)]
        
        # Then pick the z neighbour in either direction
        screenshot_above = self.find_neighbour(source_screenshot, 0, self.tile_step_size)
        screenshot_below = self.find_neighbour(source_screenshot, 0, -self.tile_step_size)
        
        # Pick the first non None neighbour
        neighbour_screenshot = screenshot_above if screenshot_above is not None else screenshot_below
        if neighbour_screenshot is None:
            raise RuntimeError(f"No neighbours found at coordinate {source_screenshot.xCoordWS}, {source_screenshot.zCoordWS}")
        
        # now get the size of the screenshot
        width, height = source_screenshot.screenshot_image.size
        
        # We now want to find the best matching row in the neighbour_screenshot for the source screenshot source row
        
        source_row = 0 if screenshot_above is not None else height - 1
        print(f"source_screenshot: {source_screenshot}")
        print(f"source_row: {source_row}")
        print(f"neighbour_screenshot: {neighbour_screenshot}")
        print(f"neighbour_screenshot direction: {'above' if screenshot_above is not None else 'below'}")
        
        best_match_index, best_match_score, all_scores = self.find_best_matching_row(source_screenshot, source_row, neighbour_screenshot)
        print(f"Best match row: {best_match_index}, Best match score: {best_match_score}")
        
        # Visualize the match
        visualization = self.visualize_match(source_screenshot.screenshot_filepath, source_row, neighbour_screenshot.screenshot_filepath, best_match_index)
        visualization.show()
    
    def find_neighbour(self, screenshot: Screenshot, x_offset: int, z_offset: int) -> Screenshot:
        x = screenshot.xCoordWS + x_offset
        z = screenshot.zCoordWS + z_offset
        neighbour_coordinate_lookup = Screenshot.make_coordinate_string(x, z)
        try:
            return self.mapped_screenshots[neighbour_coordinate_lookup]
        except KeyError:
            return None 

    def find_highest_detail_screenshot(self) -> tuple[Screenshot, float]:
        max_detail = ()
        for screenshot in self.screenshots:
            detail = self.measure_detail(screenshot)
            print(f"detail for {screenshot.xCoordWS}, {screenshot.zCoordWS}: {detail}")
            if max_detail == () or detail > max_detail[1]:
                max_detail = (screenshot, detail)
        return max_detail
    
    def measure_detail(self, screenshot: Screenshot) -> float:
        """
        Measure the detail of a screenshot by calculating the average pixel intensity of the edge image
        """
        image_path = screenshot.screenshot_filepath
        img = Image.open(image_path)
        # Convert to grayscale for edge detection
        gray_img = img.convert("L")
        # Apply edge detection filter
        edge_img = gray_img.filter(ImageFilter.FIND_EDGES)
        # Calculate the average pixel value in the edge image
        edge_intensity = sum(edge_img.getdata()) / (edge_img.width * edge_img.height)
        return edge_intensity

    
    def frequency_analysis(self, screenshot: Screenshot):
        image_path = screenshot.screenshot_filepath
        img = Image.open(image_path).convert("L")
        img_array = np.array(img)
        
        # Apply FFT to get frequency domain representation
        f_transform = fftpack.fft2(img_array)
        f_transform_shifted = fftpack.fftshift(f_transform)
        
        # Calculate magnitudes of frequency components
        magnitude = np.abs(f_transform_shifted)
        
        # Separate high frequency components (center is low frequency)
        h, w = magnitude.shape
        center_h, center_w = h//2, w//2
        
        # Create a mask for high frequency components
        radius = min(center_h, center_w) // 3  # Adjust this threshold as needed
        y, x = np.ogrid[:h, :w]
        mask = ((y - center_h)**2 + (x - center_w)**2) > radius**2
        
        # Calculate high frequency energy
        high_freq_energy = np.sum(magnitude * mask)
        total_energy = np.sum(magnitude)
        
        return high_freq_energy / total_energy


    def find_best_matching_row(self, source_screenshot: Screenshot, source_row_index: int, target_screenshot: Screenshot):
        """
        Find the best matching row in the target image for a given row from the source image.
        
        Parameters:
        -----------
        source_image_path : str
            Path to the source image
        target_image_path : str
            Path to the target image where we want to find the matching row
        source_row_index : int
            Index of the row in the source image to match
        metric : str, optional
            Similarity metric to use ('mse', 'ncc', or 'sad')
            
        Returns:
        --------
        best_match_index : int
            Index of the best matching row in the target image
        best_match_score : float
            Score of the best match (interpretation depends on metric)
        all_scores : list
            List of scores for all rows in the target image
        """
        
        source_img = source_screenshot.screenshot_image
        target_img = target_screenshot.screenshot_image
        
        # Convert to numpy arrays
        source_array = np.array(source_img)
        target_array = np.array(target_img)
        
        # Ensure images have the same width or resize if needed
        if source_array.shape[1] != target_array.shape[1]:
            print(f"Warning: Images have different widths. Source: {source_array.shape[1]}, Target: {target_array.shape[1]}")
            # Resize target to match source width if needed
            target_img = target_img.resize((source_img.width, target_img.height))
            target_array = np.array(target_img)
        
        # Extract the source row
        source_row = source_array[source_row_index]
        
        # Calculate scores for each row in the target image
        scores = []
        
        for i in range(target_array.shape[0]):
            target_row = target_array[i]
            
            # use normalized cross-correlation (NCC) (higher is better)
            
            # Handle multi-channel images
            if len(source_row.shape) > 1:
                # Calculate NCC for each channel and average
                channel_scores = []
                for c in range(source_row.shape[-1]):
                    src = source_row[..., c].astype(float)
                    tgt = target_row[..., c].astype(float)
                    
                    src_norm = src - np.mean(src)
                    tgt_norm = tgt - np.mean(tgt)
                    
                    numerator = np.sum(src_norm * tgt_norm)
                    denominator = np.sqrt(np.sum(src_norm**2) * np.sum(tgt_norm**2))
                    
                    # Avoid division by zero
                    if denominator == 0:
                        channel_scores.append(0)
                    else:
                        channel_scores.append(numerator / denominator)
                
                score = np.mean(channel_scores)
            else:
                src = source_row.astype(float)
                tgt = target_row.astype(float)
                
                src_norm = src - np.mean(src)
                tgt_norm = tgt - np.mean(tgt)
                
                numerator = np.sum(src_norm * tgt_norm)
                denominator = np.sqrt(np.sum(src_norm**2) * np.sum(tgt_norm**2))
                
                # Avoid division by zero
                if denominator == 0:
                    score = 0
                else:
                    score = numerator / denominator
                    
            scores.append(score)
        
        # NCC, higher is better
        best_match_index = np.argmax(scores)
        best_match_score = scores[best_match_index]
        
        return best_match_index, best_match_score, scores

    def visualize_match(self, source_image_path, source_row, target_image_path, target_row):
        """
        Create a visualization of the matching rows from both images.
        
        Parameters:
        -----------
        source_image_path : str
            Path to the source image
        target_image_path : str
            Path to the target image
        source_row : int
            Index of the row in the source image
        target_row : int
            Index of the matching row in the target image
            
        Returns:
        --------
        visualization : PIL.Image
            An image highlighting the matching rows in both images
        """
        # Open images
        source_img = Image.open(source_image_path)
        target_img = Image.open(target_image_path)
        
        # Convert to numpy arrays for manipulation
        source_array = np.array(source_img)
        target_array = np.array(target_img)
        
        # Create copies for highlighting
        source_highlight = source_array.copy()
        target_highlight = target_array.copy()
        
        # Highlight the rows (make them red for visibility)
        if len(source_array.shape) == 3:  # Color image
            source_highlight[source_row, :, 0] = 255  # Red channel
            source_highlight[source_row, :, 1] = 0    # Green channel
            source_highlight[source_row, :, 2] = 0    # Blue channel
            
            target_highlight[target_row, :, 0] = 255
            target_highlight[target_row, :, 1] = 0
            target_highlight[target_row, :, 2] = 0
        else:  # Grayscale image
            source_highlight[source_row, :] = 255
            target_highlight[target_row, :] = 255
        
        # Create a new image to show both side by side
        if len(source_array.shape) == 3:
            height = max(source_array.shape[0], target_array.shape[0])
            width = source_array.shape[1] + target_array.shape[1]
            visualization = np.zeros((height, width, 3), dtype=np.uint8)
            
            visualization[:source_array.shape[0], :source_array.shape[1]] = source_highlight
            visualization[:target_array.shape[0], source_array.shape[1]:] = target_highlight
        else:
            height = max(source_array.shape[0], target_array.shape[0])
            width = source_array.shape[1] + target_array.shape[1]
            visualization = np.zeros((height, width), dtype=np.uint8)
            
            visualization[:source_array.shape[0], :source_array.shape[1]] = source_highlight
            visualization[:target_array.shape[0], source_array.shape[1]:] = target_highlight
        
        return Image.fromarray(visualization)

    def launch_alignment_gui(self):
        """Launch the tile alignment GUI tool."""
        root = tk.Tk()
        try:
            from PIL import ImageDraw  # Import here to avoid import errors if not using GUI
            gui = TileAlignmentGUI(root, self)
            root.mainloop()
        except Exception as e:
            print(f"Error launching GUI: {e}")
            root.destroy()


if __name__ == "__main__":
    # Set up the args - We take an input directory to locate the screenshots and write tiles next to them
    parser = argparse.ArgumentParser(description="Center crop screenshots to a given resolution")
    parser.add_argument("input_dir", help="The directory containing the screenshots to crop")
    parser.add_argument("output_dir", help="The directory containing the screenshots to crop")
    parser.add_argument("-f", "--find-crop", help="Automatically find the crop size", action="store_true")
    parser.add_argument("-m", "--make_map", help="Create a large map from the screenshots instead of the final tiles", action="store_true")
    parser.add_argument("-g", "--gui", help="Launch the tile alignment GUI", action="store_true")
    args = parser.parse_args()

    print(f"Processing screenshots in {args.input_dir}")
    screenshot_processor = ScreenshotProcessor.from_directory(args.input_dir)
    
    if args.find_crop:
        print("Finding the crop size")
        screenshot_processor.auto_find_crop()
        sys.exit(0)
    
    if args.gui:
        print("Launching alignment GUI")
        screenshot_processor.launch_alignment_gui()
        sys.exit(0)
    
    screenshot_processor.make_tiles() # Will also delete the original screenshots if DELETE_ORIGINALS is True

    if args.make_map:
        print("Making large test map")
        screenshot_processor.make_large_map(os.path.join(args.output_dir, "test_map.png"))
    else:
        print("Creating initial tiles")
        screenshot_processor.make_initial_tiles(args.output_dir, 0)

